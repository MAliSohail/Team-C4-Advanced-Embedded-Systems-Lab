from __future__ import annotations

import os
from pathlib import Path

PI_PORT = int(os.environ.get("PI_PORT", "5000"))
ARDUINO_BASE_URL = os.environ.get("ARDUINO_BASE_URL", "http://192.168.0.103")
MQTT_HOST = os.environ.get("MQTT_BROKER_HOST", "127.0.0.1")
MQTT_PORT = int(os.environ.get("MQTT_BROKER_PORT", "1883"))
CAMERA_ID = os.environ.get("CAMERA_DEVICE_ID", "qc-camera-01")
DATA_DIR = Path(os.environ.get("INSPECTION_ROOT", "data/inspections"))

ANGLES = (0, 60, 120, 180)
ARDUINO_TIMEOUT = 5
CAMERA_ACK_TIMEOUT = 3
CAMERA_RESULT_TIMEOUT = 15
START_DELAY = 0.6

CLASSIFIER_MODEL_PATH = os.environ.get("CLASSIFIER_MODEL_PATH", "model.tflite")
CLASSIFIER_LABELS = ("PASS", "REJECT")
CLASSIFIER_INPUT_SIZE = 224

CAMERA_ROOT = f"qc/camera/{CAMERA_ID}"
CAMERA_COMMAND_TOPIC = f"{CAMERA_ROOT}/command"
CAMERA_EVENT_TOPIC = f"{CAMERA_ROOT}/event"
CAMERA_STATUS_TOPIC = f"{CAMERA_ROOT}/status"
CAMERA_HEARTBEAT_TOPIC = f"{CAMERA_ROOT}/heartbeat"
