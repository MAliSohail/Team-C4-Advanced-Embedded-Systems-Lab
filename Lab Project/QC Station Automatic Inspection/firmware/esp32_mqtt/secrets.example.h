#pragma once

// Wi-Fi used by the Pi, ESP32-CAM and Arduino.
#define WIFI_SSID "REPLACE_WITH_YOUR_WIFI_NAME"
#define WIFI_PASSWORD "REPLACE_WITH_YOUR_WIFI_PASSWORD"

// Raspberry Pi address used for JPEG HTTP upload.
// Use only the IP/hostname — do not include http:// or :5000.
#define PI_HOST "192.168.0.REPLACE"
#define PI_PORT 5000

// Mosquitto runs on the same Raspberry Pi in Stage 2.
// Use only the IP/hostname — do not include mqtt:// or :1883.
#define MQTT_HOST "192.168.0.REPLACE"
#define MQTT_PORT 1883
