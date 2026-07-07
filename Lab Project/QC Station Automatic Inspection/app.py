from __future__ import annotations

import copy
import logging
import threading
import time
import uuid
from datetime import datetime
from typing import Any

from flask import Flask, jsonify, render_template, request, send_file

import config
from services.arduino import ArduinoClient
from services.camera_mqtt import CameraMqtt
from services.classifier import DefectClassifier
from services.storage import InspectionStorage

app = Flask(__name__)
logging.basicConfig(level=logging.INFO, format="%(asctime)s [%(levelname)s] %(message)s")

storage = InspectionStorage(config.DATA_DIR)
arduino = ArduinoClient(config.ARDUINO_BASE_URL, config.ARDUINO_TIMEOUT)
camera = CameraMqtt(
    config.MQTT_HOST,
    config.MQTT_PORT,
    config.CAMERA_COMMAND_TOPIC,
    config.CAMERA_EVENT_TOPIC,
    config.CAMERA_STATUS_TOPIC,
    config.CAMERA_HEARTBEAT_TOPIC,
)
classifier = DefectClassifier(
    config.CLASSIFIER_MODEL_PATH,
    config.CLASSIFIER_LABELS,
    config.CLASSIFIER_INPUT_SIZE,
)

lock = threading.RLock()
station: dict[str, Any] = {
    "busy": False,
    "active": None,
    "current": None,
    "last": None,
}


def now_iso() -> str:
    return datetime.now().astimezone().isoformat(timespec="seconds")


def public(record: dict[str, Any] | None) -> dict[str, Any] | None:
    return copy.deepcopy(record) if record else None


def save_and_publish(record: dict[str, Any], **changes: Any) -> None:
    record.update(changes)
    storage.save(record)
    with lock:
        station["current"] = public(record)


def finish(record: dict[str, Any]) -> None:
    with lock:
        station["busy"] = False
        station["active"] = None
        station["current"] = public(record)
        station["last"] = public(record)


def create_record(source: str) -> dict[str, Any]:
    return {
        "inspection_id": storage.new_id(),
        "source": source,
        "state": "CAPTURING",
        "message": "Inspection started",
        "started_at": now_iso(),
        "completed_at": None,
        "current_view": 0,
        "current_angle": 0,
        "completed_views": 0,
        "views": [],
        "final_result": None,
        "error": None,
    }


def run_inspection(record: dict[str, Any]) -> None:
    time.sleep(config.START_DELAY)
    try:
        if not camera.connected:
            raise RuntimeError("MQTT broker is offline")
        if not camera.camera_online:
            raise RuntimeError("ESP32-CAM is offline")

        arduino.home()
        for view, angle in enumerate(config.ANGLES, start=1):
            command_id = f"cmd-{record['inspection_id']}-v{view}-{uuid.uuid4().hex[:6]}"
            save_and_publish(
                record,
                state="CAPTURING",
                message=f"Capturing view {view} of 4",
                current_view=view,
                current_angle=angle,
                current_command_id=command_id,
            )

            arduino.move(angle)
            camera.capture(
                {
                    "command": "capture",
                    "command_id": command_id,
                    "inspection_id": record["inspection_id"],
                    "view": view,
                    "angle": angle,
                },
                config.CAMERA_ACK_TIMEOUT,
                config.CAMERA_RESULT_TIMEOUT,
            )

            image_path = storage.image_path(record["inspection_id"], view, angle)
            if not image_path.exists():
                raise RuntimeError(f"Image for view {view} was not saved")

            prediction = classifier.classify(image_path)
            record["views"].append(
                {
                    "view": view,
                    "angle": angle,
                    "filename": image_path.name,
                    "classification": {
                        "label": prediction["label"],
                        "source": "model",
                        "confidence": prediction["confidence"],
                        "scores": prediction["scores"],
                        "reviewed_at": now_iso(),
                    },
                }
            )
            save_and_publish(
                record,
                completed_views=view,
                message=f"View {view} classified as {prediction['label']}",
            )

        arduino.home()
        result = "REJECT" if any(
            item["classification"]["label"] == "REJECT" for item in record["views"]
        ) else "PASS"
        save_and_publish(
            record,
            state="COMPLETE",
            message=f"Inspection result: {result}",
            final_result=result,
            completed_at=now_iso(),
            current_view=0,
            current_angle=0,
            current_command_id=None,
        )
        finish(record)
    except Exception as exc:
        logging.exception("Inspection failed")
        try:
            arduino.home()
        except Exception:
            pass
        save_and_publish(
            record,
            state="ERROR",
            message="Inspection failed",
            final_result="SYSTEM_ERROR",
            completed_at=now_iso(),
            error=str(exc),
        )
        finish(record)


def start_inspection(source: str) -> tuple[dict[str, Any], int]:
    with lock:
        if station["busy"]:
            return {"accepted": False, "error": "station_busy"}, 409
        if not camera.connected:
            return {"accepted": False, "error": "mqtt_offline"}, 503
        if not camera.camera_online:
            return {"accepted": False, "error": "camera_offline"}, 503

        record = create_record(source)
        station["busy"] = True
        station["active"] = record
        station["current"] = public(record)

    storage.save(record)
    threading.Thread(target=run_inspection, args=(record,), daemon=True).start()
    return {"accepted": True, "inspection_id": record["inspection_id"]}, 202


def active_record(inspection_id: str) -> dict[str, Any] | None:
    with lock:
        record = station["active"]
        if record and record["inspection_id"] == inspection_id:
            return record
    return None


@app.get("/")
def dashboard():
    return render_template("dashboard.html")


@app.get("/api/status")
def status():
    with lock:
        current = public(station["current"])
        busy = station["busy"]
    return jsonify(
        {
            "busy": busy,
            "inspection": current,
            "devices": {
                "pi": True,
                "arduino": arduino.online,
                "mqtt": camera.connected,
                "camera": camera.camera_online,
            },
        }
    )


@app.post("/api/inspection/start")
def start_route():
    source = request.headers.get("X-Event-Source", request.remote_addr or "unknown")
    body, status_code = start_inspection(source)
    return jsonify(body), status_code


@app.post("/api/inspection/<inspection_id>/image")
def receive_image(inspection_id: str):
    record = active_record(inspection_id)
    if record is None:
        return jsonify({"error": "no_active_inspection"}), 409

    try:
        view = int(request.headers.get("X-View-Index", ""))
        angle = int(request.headers.get("X-Servo-Angle", ""))
    except ValueError:
        return jsonify({"error": "invalid_view_or_angle"}), 400

    command_id = request.headers.get("X-Command-ID", "")
    if view not in range(1, 5) or angle != config.ANGLES[view - 1]:
        return jsonify({"error": "unexpected_view_or_angle"}), 400
    if record.get("current_view") != view or record.get("current_command_id") != command_id:
        return jsonify({"error": "unexpected_camera_upload"}), 409
    if request.mimetype != "image/jpeg":
        return jsonify({"error": "jpeg_required"}), 415

    image = request.get_data()
    if not image.startswith(b"\xff\xd8") or not image.endswith(b"\xff\xd9"):
        return jsonify({"error": "invalid_jpeg"}), 400

    destination = storage.image_path(inspection_id, view, angle)
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = destination.with_suffix(".jpg.tmp")
    temporary.write_bytes(image)
    temporary.replace(destination)
    return jsonify({"accepted": True, "bytes": len(image)}), 201


@app.get("/api/inspection/<inspection_id>/image/<int:view>")
def inspection_image(inspection_id: str, view: int):
    with lock:
        records = [station["current"], station["last"]]
    record = next(
        (item for item in records if item and item["inspection_id"] == inspection_id),
        None,
    )
    if record is None:
        return jsonify({"error": "inspection_not_found"}), 404
    view_data = next((item for item in record["views"] if item["view"] == view), None)
    if view_data is None:
        return jsonify({"error": "view_not_found"}), 404

    path = storage.directory(inspection_id) / view_data["filename"]
    response = send_file(path.resolve(), mimetype="image/jpeg", max_age=0)
    response.headers["Cache-Control"] = "no-store, max-age=0"
    return response


@app.get("/api/health")
def health():
    return jsonify({"status": "ok"})


if __name__ == "__main__":
    camera.start()
    app.run(host="0.0.0.0", port=config.PI_PORT, debug=False, threaded=True)
