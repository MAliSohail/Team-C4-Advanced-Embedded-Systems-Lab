# QC Station

A Flask-based inspection station that captures four views of a part and classifies each one automatically using a trained image model.

## How it works

1. An operator places a part on the turntable and presses the Arduino button.
2. The Arduino notifies the Raspberry Pi over HTTP.
3. The Pi drives the Arduino turntable to four fixed angles (0, 60, 120, 180 degrees).
4. At each angle the Pi sends a capture command to the ESP32-CAM over MQTT.
5. The ESP32-CAM takes a photo and uploads the original JPEG to the Pi over HTTP.
6. The Pi runs the image through a TFLite image classification model and records a PASS or REJECT label with a confidence score for that view.
7. Once all four views are captured and classified, the Pi computes the final result. Any rejected view produces a final REJECT; four passed views produce a final PASS.
8. Hardware or communication failures at any stage produce a SYSTEM_ERROR result instead.
9. Every inspection is saved in its own folder with a `result.json` containing the full record.

The dashboard shows live status, the four captured images, and each view's classification and confidence as the inspection runs.

## Project structure

```text
qc_station_stage4_minimal/
├── app.py                     # Flask routes and inspection sequence
├── config.py                  # IP addresses, ports, topics, timeouts, model settings
├── model.tflite                # Trained image classification model
├── services/
│   ├── arduino.py              # Arduino HTTP client
│   ├── camera_mqtt.py          # MQTT capture command handling
│   ├── classifier.py           # TFLite inference and image preprocessing
│   └── storage.py              # Inspection folders and result.json
├── templates/dashboard.html    # Page structure
├── static/dashboard.css        # Dashboard appearance
├── static/dashboard.js         # Live status and image updates
├── firmware/
│   ├── arduino_http/
│   └── esp32_mqtt/
├── requirements.txt
├── setup.sh
└── start.sh
```

## Installation on Raspberry Pi

Copy or extract the folder into `~/qc-station/stage4-minimal`, then run:

```bash
cd ~/qc-station/stage4-minimal
chmod +x setup.sh start.sh
./setup.sh
cp config.env.example config.env
nano config.env
```

Replace the Arduino IP in `config.env`.

Confirm Mosquitto is running:

```bash
sudo systemctl status mosquitto --no-pager
```

Stop any previous Pi server and start this one:

```bash
pkill -f 'python app.py' || true
./start.sh
```

Open the dashboard from the laptop:

```text
http://PI_IP:5000
```

## Firmware

No architecture change is required. The ZIP includes copies of the working HTTP Arduino sketch and MQTT ESP32-CAM sketch.

For each sketch:

1. Copy `secrets.example.h` to `secrets.h`.
2. Enter the current Wi-Fi and Pi addresses.
3. Upload through Arduino IDE.

The ESP32 copy includes the two-frame flush used to prevent a stale first image.

## Classification model

The station uses a TFLite model (`model.tflite`) exported from Teachable Machine. Input is a 224x224 RGB image, and output is a two-class softmax score (PASS, REJECT).

Preprocessing before inference:

1. Center-crop the captured JPEG to a square.
2. Resize to 224x224 using nearest-neighbor interpolation.
3. Scale pixel values to the range -1 to 1.

This matches the preprocessing built into Teachable Machine's export pipeline, so images are fed to the model exactly as captured, with no manual editing.

To use a different model, replace `model.tflite` and update `CLASSIFIER_MODEL_PATH`, `CLASSIFIER_LABELS`, and `CLASSIFIER_INPUT_SIZE` in `config.py` to match.

## Main API routes

```text
GET  /
GET  /api/health
GET  /api/status
POST /api/inspection/start
POST /api/inspection/<id>/image
GET  /api/inspection/<id>/image/<view>
```

## Expected result folder

```text
data/inspections/QC-YYYYMMDD-HHMMSS-xxx/
├── view_1_000deg.jpg
├── view_2_060deg.jpg
├── view_3_120deg.jpg
├── view_4_180deg.jpg
└── result.json
```

Each view in `result.json` records its classification label, confidence, and raw scores from the model.

## Deliberately removed

- Retake controls
- Inspection-history UI
- Detailed timing records
- Large event logs
- Complex state list
- BLE experiment

## Important limitation

This version does not restore an unfinished inspection after the Pi server is restarted. That can be added later as a separate reliability feature if required.
