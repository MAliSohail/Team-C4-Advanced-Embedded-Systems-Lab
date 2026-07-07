from __future__ import annotations

import json
import logging
import threading
from dataclasses import dataclass, field
from typing import Any

import paho.mqtt.client as mqtt


@dataclass
class PendingCommand:
    acknowledged: threading.Event = field(default_factory=threading.Event)
    completed: threading.Event = field(default_factory=threading.Event)
    result: dict[str, Any] | None = None


class CameraMqtt:
    def __init__(
        self,
        host: str,
        port: int,
        command_topic: str,
        event_topic: str,
        status_topic: str,
        heartbeat_topic: str,
    ) -> None:
        self.host = host
        self.port = port
        self.command_topic = command_topic
        self.event_topic = event_topic
        self.status_topic = status_topic
        self.heartbeat_topic = heartbeat_topic
        self.connected = False
        self.camera_online = False
        self.last_error: str | None = None
        self._pending: dict[str, PendingCommand] = {}
        self._lock = threading.Lock()

        self.client = mqtt.Client(
            callback_api_version=mqtt.CallbackAPIVersion.VERSION2,
            client_id="qc-pi-minimal",
        )
        self.client.on_connect = self._on_connect
        self.client.on_disconnect = self._on_disconnect
        self.client.on_message = self._on_message
        self.client.reconnect_delay_set(1, 10)

    def start(self) -> None:
        self.client.connect_async(self.host, self.port, 30)
        self.client.loop_start()

    def _on_connect(self, client, userdata, flags, reason_code, properties) -> None:
        self.connected = reason_code == 0
        if not self.connected:
            self.last_error = f"MQTT connection rejected: {reason_code}"
            return
        self.last_error = None
        client.subscribe(
            [
                (self.event_topic, 1),
                (self.status_topic, 1),
                (self.heartbeat_topic, 0),
            ]
        )
        logging.info("Connected to MQTT broker")

    def _on_disconnect(self, client, userdata, flags, reason_code, properties) -> None:
        self.connected = False
        if reason_code != 0:
            self.last_error = f"MQTT disconnected: {reason_code}"

    def _on_message(self, client, userdata, message) -> None:
        try:
            payload = json.loads(message.payload.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError):
            return

        if message.topic in {self.status_topic, self.heartbeat_topic}:
            self.camera_online = bool(payload.get("online", True))
            return

        if message.topic != self.event_topic:
            return

        command_id = str(payload.get("command_id", ""))
        event = payload.get("event")
        with self._lock:
            pending = self._pending.get(command_id)
        if pending is None:
            return

        if event in {"command_accepted", "capture_started"}:
            pending.acknowledged.set()
        if event in {"capture_complete", "capture_failed", "command_rejected"}:
            pending.result = payload
            pending.acknowledged.set()
            pending.completed.set()

    def capture(
        self,
        command: dict[str, Any],
        ack_timeout: int,
        result_timeout: int,
    ) -> dict[str, Any]:
        if not self.connected:
            raise RuntimeError("MQTT broker is offline")

        command_id = command["command_id"]
        pending = PendingCommand()
        with self._lock:
            self._pending[command_id] = pending

        try:
            encoded = json.dumps(command, separators=(",", ":"))
            # Two sends with the same command ID are safe because the ESP32
            # rejects duplicate work and repeats its cached acknowledgement.
            for attempt in range(2):
                info = self.client.publish(self.command_topic, encoded, qos=1)
                info.wait_for_publish(timeout=ack_timeout)
                if pending.acknowledged.wait(ack_timeout):
                    break
                if attempt == 1:
                    raise RuntimeError("Camera did not acknowledge the MQTT command")

            if not pending.completed.wait(result_timeout):
                raise RuntimeError("Camera capture timed out")

            result = pending.result or {}
            if result.get("event") != "capture_complete" or not result.get("success"):
                raise RuntimeError(str(result.get("error", "Camera capture failed")))
            return result
        finally:
            with self._lock:
                self._pending.pop(command_id, None)
