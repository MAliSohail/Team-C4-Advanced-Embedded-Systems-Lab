# QC Station — Minimal Stage 4

This is the reduced Flask implementation of the inspection station. It keeps the required workflow and removes the optional features that made the earlier server too large.

## Included functionality

- Arduino button starts an inspection through HTTP.
- Raspberry Pi controls Arduino `/home` and `/move` routes through HTTP.
- Raspberry Pi sends four camera commands through MQTT.
- ESP32-CAM uploads original JPEG bytes through HTTP.
- Dashboard shows the four images.
- Technician marks every image PASS or REJECT.
- Any rejected view produces a final REJECT; four passed views produce PASS.
- Hardware or communication failures produce SYSTEM_ERROR.
- Every inspection is saved in its own folder with `result.json`.

## Deliberately removed

- Retake controls
- Pass-all and reset-review controls
- Inspection-history UI
- Reviewer names and notes
- Detailed timing records
- Large event logs
- Complex state list
- PyTorch/stub classifier
- BLE experiment

## Project structure

```text
qc_station_stage4_minimal/
├── app.py                     # Flask routes and inspection sequence
├── config.py                  # IP addresses, ports, topics and timeouts
├── services/
│   ├── arduino.py             # Arduino HTTP client
│   ├── camera_mqtt.py         # MQTT capture command handling
│   └── storage.py             # Inspection folders and result.json
├── templates/dashboard.html   # Page structure
├── static/dashboard.css       # Dashboard appearance
├── static/dashboard.js        # Live updates and PASS/REJECT actions
├── firmware/
│   ├── arduino_http/
│   └── esp32_mqtt/
├── requirements.txt
├── setup.sh
└── start.sh
```

## Installation on Raspberry Pi

Copy/extract the folder into `~/qc-station/stage4-minimal`, then run:

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
pkill -f pi_server.py || true
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

## Main API routes

```text
GET  /
GET  /api/health
GET  /api/status
POST /api/inspection/start
POST /api/inspection/<id>/image
GET  /api/inspection/<id>/image/<view>
POST /api/inspection/<id>/classify/<view>
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

## Important limitation

This minimal version does not restore an unfinished inspection after the Pi server is restarted. That can be added later as a separate reliability feature if required.
