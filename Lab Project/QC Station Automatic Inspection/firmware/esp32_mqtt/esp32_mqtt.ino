/*
 * QC Station — ESP32-CAM Stage 2
 *
 * Board:
 *   AI-Thinker ESP32-CAM
 *
 * Stage 2 responsibilities:
 *   1. Connect to the Mosquitto broker running on the Raspberry Pi.
 *   2. Receive one capture command over MQTT.
 *   3. Publish command acknowledgement/status events over MQTT.
 *   4. Capture one JPEG.
 *   5. Upload the original JPEG bytes to the Pi over HTTP POST.
 *   6. Publish capture success/failure over MQTT.
 *
 * MQTT command topic:
 *   qc/camera/qc-camera-01/command
 *
 * MQTT outgoing topics:
 *   qc/camera/qc-camera-01/event
 *   qc/camera/qc-camera-01/status
 *   qc/camera/qc-camera-01/heartbeat
 *
 * HTTP debug routes:
 *   GET /
 *   GET /status
 *   GET /capture
 *
 * Required Arduino libraries:
 *   ArduinoMqttClient
 *   ArduinoJson (version 7)
 */

#include "esp_camera.h"
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <ArduinoMqttClient.h>
#include <ArduinoJson.h>

#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

#include "secrets.h"


// ============================================================
// AI-Thinker ESP32-CAM pin mapping
// ============================================================

#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27

#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5

#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22


// ============================================================
// Device and MQTT configuration
// ============================================================

constexpr const char *DEVICE_ID = "qc-camera-01";

constexpr const char *MQTT_COMMAND_TOPIC =
  "qc/camera/qc-camera-01/command";
constexpr const char *MQTT_EVENT_TOPIC =
  "qc/camera/qc-camera-01/event";
constexpr const char *MQTT_STATUS_TOPIC =
  "qc/camera/qc-camera-01/status";
constexpr const char *MQTT_HEARTBEAT_TOPIC =
  "qc/camera/qc-camera-01/heartbeat";

constexpr unsigned long MQTT_RECONNECT_INTERVAL_MS = 3000;
constexpr unsigned long HEARTBEAT_INTERVAL_MS = 5000;
constexpr unsigned long HTTP_UPLOAD_TIMEOUT_MS = 15000;
constexpr int MAX_UPLOAD_ATTEMPTS = 3;


// ============================================================
// Global objects and state
// ============================================================

WebServer server(80);
WiFiClient mqttNetworkClient;
MqttClient mqttClient(mqttNetworkClient);

bool cameraReady = false;
bool captureBusy = false;

unsigned long successfulCaptures = 0;
unsigned long failedCaptures = 0;
unsigned long lastMqttConnectAttemptMs = 0;
unsigned long lastHeartbeatMs = 0;

String activeCommandId = "";
String lastCommandId = "";
String lastTerminalPayload = "";


struct CaptureCommand {
  bool pending = false;
  String commandId = "";
  String inspectionId = "";
  int view = 0;
  int angle = 0;
};

CaptureCommand queuedCommand;


// ============================================================
// JSON and MQTT helpers
// ============================================================

String serializeDocument(JsonDocument &document) {
  String payload;
  serializeJson(document, payload);
  return payload;
}


bool publishPayload(
  const char *topic,
  const String &payload,
  int qos,
  bool retained
) {
  if (!mqttClient.connected()) {
    Serial.print("Cannot publish because MQTT is offline: ");
    Serial.println(topic);
    return false;
  }

  bool duplicate = false;

  if (!mqttClient.beginMessage(
        topic,
        payload.length(),
        retained,
        qos,
        duplicate
      )) {
    Serial.print("MQTT beginMessage failed: ");
    Serial.println(topic);
    return false;
  }

  size_t written = mqttClient.print(payload);

  if (written != payload.length()) {
    Serial.println("MQTT payload write was incomplete");
    mqttClient.stop();
    return false;
  }

  int result = mqttClient.endMessage();

  if (result != 1) {
    Serial.print("MQTT publish failed: ");
    Serial.println(topic);
    return false;
  }

  return true;
}


String makeCommandEvent(
  const char *eventName,
  const CaptureCommand &command,
  bool success
) {
  JsonDocument document;

  document["device_id"] = DEVICE_ID;
  document["event"] = eventName;
  document["success"] = success;
  document["command_id"] = command.commandId;
  document["inspection_id"] = command.inspectionId;
  document["view"] = command.view;
  document["angle"] = command.angle;
  document["uptime_ms"] = millis();

  return serializeDocument(document);
}


void publishAccepted(
  const CaptureCommand &command,
  bool duplicate
) {
  JsonDocument document;

  document["device_id"] = DEVICE_ID;
  document["event"] = "command_accepted";
  document["success"] = true;
  document["command_id"] = command.commandId;
  document["inspection_id"] = command.inspectionId;
  document["view"] = command.view;
  document["angle"] = command.angle;
  document["duplicate"] = duplicate;
  document["uptime_ms"] = millis();

  String payload = serializeDocument(document);
  publishPayload(MQTT_EVENT_TOPIC, payload, 1, false);
}


void publishRejected(
  const CaptureCommand &command,
  const char *error
) {
  JsonDocument document;

  document["device_id"] = DEVICE_ID;
  document["event"] = "command_rejected";
  document["success"] = false;
  document["command_id"] = command.commandId;
  document["inspection_id"] = command.inspectionId;
  document["view"] = command.view;
  document["angle"] = command.angle;
  document["error"] = error;
  document["uptime_ms"] = millis();

  String payload = serializeDocument(document);
  publishPayload(MQTT_EVENT_TOPIC, payload, 1, false);
}


void publishStatus(
  bool online,
  const char *reason
) {
  JsonDocument document;

  document["device_id"] = DEVICE_ID;
  document["online"] = online;
  document["reason"] = reason;
  document["camera_ready"] = cameraReady;
  document["capture_busy"] = captureBusy;
  document["wifi_connected"] = WiFi.status() == WL_CONNECTED;
  document["mqtt_connected"] = mqttClient.connected();
  document["ip"] = WiFi.localIP().toString();
  document["rssi_dbm"] = WiFi.RSSI();
  document["successful_captures"] = successfulCaptures;
  document["failed_captures"] = failedCaptures;
  document["uptime_ms"] = millis();

  String payload = serializeDocument(document);
  publishPayload(MQTT_STATUS_TOPIC, payload, 1, true);
}


void publishHeartbeat() {
  JsonDocument document;

  document["device_id"] = DEVICE_ID;
  document["online"] = true;
  document["camera_ready"] = cameraReady;
  document["capture_busy"] = captureBusy;
  document["wifi_connected"] = WiFi.status() == WL_CONNECTED;
  document["mqtt_connected"] = mqttClient.connected();
  document["rssi_dbm"] = WiFi.RSSI();
  document["free_heap"] = ESP.getFreeHeap();
  document["uptime_ms"] = millis();

  String payload = serializeDocument(document);
  publishPayload(MQTT_HEARTBEAT_TOPIC, payload, 0, false);
}


// ============================================================
// Camera initialization
// ============================================================

bool initializeCamera() {
  camera_config_t config;

  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;

  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;

  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;

  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;

  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;

  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.grab_mode = CAMERA_GRAB_LATEST;

  if (psramFound()) {
    config.frame_size = FRAMESIZE_UXGA;
    config.jpeg_quality = 10;
    config.fb_count = 1;
  } else {
    config.frame_size = FRAMESIZE_SVGA;
    config.jpeg_quality = 12;
    config.fb_count = 1;
  }

  esp_err_t error = esp_camera_init(&config);

  if (error != ESP_OK) {
    Serial.printf(
      "Camera initialization failed: 0x%x\n",
      error
    );
    return false;
  }

  return true;
}


// ============================================================
// Wi-Fi and MQTT connection handling
// ============================================================

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.print("Connecting to WiFi");

  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    delay(500);
  }

  Serial.println();
  Serial.println("WiFi connected");

  Serial.print("ESP32-CAM IP: ");
  Serial.println(WiFi.localIP());

  Serial.print("Signal strength: ");
  Serial.print(WiFi.RSSI());
  Serial.println(" dBm");
}


void configureMqttWill() {
  JsonDocument document;

  document["device_id"] = DEVICE_ID;
  document["online"] = false;
  document["reason"] = "unexpected_disconnect";

  String willPayload = serializeDocument(document);

  mqttClient.beginWill(
    MQTT_STATUS_TOPIC,
    willPayload.length(),
    true,
    1
  );
  mqttClient.print(willPayload);
  mqttClient.endWill();
}


void connectMqttIfNeeded() {
  if (mqttClient.connected()) {
    return;
  }

  unsigned long now = millis();

  if (
    now - lastMqttConnectAttemptMs <
    MQTT_RECONNECT_INTERVAL_MS
  ) {
    return;
  }

  lastMqttConnectAttemptMs = now;

  Serial.print("Connecting to MQTT broker at ");
  Serial.print(MQTT_HOST);
  Serial.print(":");
  Serial.println(MQTT_PORT);

  mqttClient.stop();

  if (!mqttClient.connect(MQTT_HOST, MQTT_PORT)) {
    Serial.print("MQTT connection failed; code: ");
    Serial.println(mqttClient.connectError());
    return;
  }

  Serial.println("MQTT connected");

  if (!mqttClient.subscribe(MQTT_COMMAND_TOPIC, 1)) {
    Serial.println("MQTT command subscription failed");
    mqttClient.stop();
    return;
  }

  Serial.print("Subscribed to: ");
  Serial.println(MQTT_COMMAND_TOPIC);

  publishStatus(true, "connected");
  lastHeartbeatMs = 0;
}


// ============================================================
// Command validation and MQTT callback
// ============================================================

bool isValidInspectionId(const String &inspectionId) {
  if (inspectionId.length() < 3 || inspectionId.length() > 80) {
    return false;
  }

  for (size_t i = 0; i < inspectionId.length(); i++) {
    char c = inspectionId.charAt(i);

    bool allowed =
      isAlphaNumeric(c) ||
      c == '-' ||
      c == '_';

    if (!allowed) {
      return false;
    }
  }

  return true;
}


bool isValidCommandId(const String &commandId) {
  if (commandId.length() < 3 || commandId.length() > 120) {
    return false;
  }

  for (size_t i = 0; i < commandId.length(); i++) {
    char c = commandId.charAt(i);

    bool allowed =
      isAlphaNumeric(c) ||
      c == '-' ||
      c == '_';

    if (!allowed) {
      return false;
    }
  }

  return true;
}


bool isAllowedViewAndAngle(
  int view,
  int angle
) {
  return (
    (view == 1 && angle == 0) ||
    (view == 2 && angle == 60) ||
    (view == 3 && angle == 120) ||
    (view == 4 && angle == 180)
  );
}


void onMqttMessage(int messageSize) {
  String topic = mqttClient.messageTopic();
  String payload;
  payload.reserve(messageSize + 1);

  while (mqttClient.available()) {
    payload += static_cast<char>(mqttClient.read());
  }

  Serial.println();
  Serial.print("MQTT message on ");
  Serial.println(topic);
  Serial.println(payload);

  if (topic != MQTT_COMMAND_TOPIC) {
    return;
  }

  JsonDocument document;
  DeserializationError error = deserializeJson(document, payload);

  CaptureCommand command;
  command.commandId = document["command_id"] | "";
  command.inspectionId = document["inspection_id"] | "";
  command.view = document["view"] | 0;
  command.angle = document["angle"] | -1;

  if (error) {
    Serial.print("Invalid command JSON: ");
    Serial.println(error.c_str());
    publishRejected(command, "invalid_json");
    return;
  }

  String commandName = document["command"] | "";

  if (commandName != "capture") {
    publishRejected(command, "unsupported_command");
    return;
  }

  if (!isValidCommandId(command.commandId)) {
    publishRejected(command, "invalid_command_id");
    return;
  }

  if (!isValidInspectionId(command.inspectionId)) {
    publishRejected(command, "invalid_inspection_id");
    return;
  }

  if (!isAllowedViewAndAngle(command.view, command.angle)) {
    publishRejected(command, "invalid_view_angle_combination");
    return;
  }

  // A retransmission after completion receives the cached terminal result.
  // This prevents a duplicate MQTT command from taking a second picture.
  if (
    command.commandId == lastCommandId &&
    lastTerminalPayload.length() > 0
  ) {
    Serial.println("Duplicate completed command; returning cached result");
    publishPayload(
      MQTT_EVENT_TOPIC,
      lastTerminalPayload,
      1,
      false
    );
    return;
  }

  // A retransmission while the same command is queued or active is
  // acknowledged again, but it is not queued twice.
  if (
    (
      queuedCommand.pending &&
      command.commandId == queuedCommand.commandId
    ) ||
    (
      captureBusy &&
      command.commandId == activeCommandId
    )
  ) {
    Serial.println("Duplicate active command; acknowledging without recapture");
    publishAccepted(command, true);
    return;
  }

  if (queuedCommand.pending || captureBusy) {
    publishRejected(command, "camera_busy");
    return;
  }

  queuedCommand = command;
  queuedCommand.pending = true;

  publishAccepted(command, false);
}


// ============================================================
// JPEG upload
// ============================================================

bool uploadJpegToPi(
  camera_fb_t *frame,
  const CaptureCommand &command,
  int &httpStatus,
  String &responseBody
) {
  if (WiFi.status() != WL_CONNECTED) {
    responseBody = "WiFi disconnected";
    httpStatus = -1;
    return false;
  }

  String uploadUrl = "http://";
  uploadUrl += PI_HOST;
  uploadUrl += ":";
  uploadUrl += String(PI_PORT);
  uploadUrl += "/api/inspection/";
  uploadUrl += command.inspectionId;
  uploadUrl += "/image";

  HTTPClient http;

  if (!http.begin(uploadUrl)) {
    responseBody = "http.begin failed";
    httpStatus = -1;
    return false;
  }

  http.setTimeout(HTTP_UPLOAD_TIMEOUT_MS);

  http.addHeader("Content-Type", "image/jpeg");
  http.addHeader("X-Device-ID", DEVICE_ID);
  http.addHeader("X-Command-ID", command.commandId);
  http.addHeader("X-View-Index", String(command.view));
  http.addHeader("X-Servo-Angle", String(command.angle));

  Serial.print("Uploading ");
  Serial.print(frame->len);
  Serial.print(" bytes to ");
  Serial.println(uploadUrl);

  httpStatus = http.POST(frame->buf, frame->len);

  if (httpStatus > 0) {
    responseBody = http.getString();
  } else {
    responseBody = http.errorToString(httpStatus);
  }

  http.end();

  return httpStatus == 201;
}


// ============================================================
// Capture processing outside the MQTT callback
// ============================================================

void processQueuedCapture() {
  if (!queuedCommand.pending) {
    return;
  }

  CaptureCommand command = queuedCommand;
  queuedCommand.pending = false;

  captureBusy = true;
  activeCommandId = command.commandId;

  publishPayload(
    MQTT_EVENT_TOPIC,
    makeCommandEvent("capture_started", command, true),
    1,
    false
  );

  Serial.println();
  Serial.println("=== MQTT capture command ===");
  Serial.print("Command ID: ");
  Serial.println(command.commandId);
  Serial.print("Inspection: ");
  Serial.println(command.inspectionId);
  Serial.print("View: ");
  Serial.println(command.view);
  Serial.print("Angle: ");
  Serial.println(command.angle);

  String terminalPayload;

  if (!cameraReady) {
    JsonDocument document;
    document["device_id"] = DEVICE_ID;
    document["event"] = "capture_failed";
    document["success"] = false;
    document["command_id"] = command.commandId;
    document["inspection_id"] = command.inspectionId;
    document["view"] = command.view;
    document["angle"] = command.angle;
    document["error"] = "camera_not_ready";
    document["uptime_ms"] = millis();
    terminalPayload = serializeDocument(document);
  } else {
    // Flush two buffered frames so the first image of a new inspection is fresh.
    for (int i = 0; i < 2; i++) {
      camera_fb_t *staleFrame = esp_camera_fb_get();
      if (staleFrame) {
        esp_camera_fb_return(staleFrame);
      }
      mqttClient.poll();
      delay(100);
    }

    camera_fb_t *frame = esp_camera_fb_get();

    if (!frame) {
      failedCaptures++;

      JsonDocument document;
      document["device_id"] = DEVICE_ID;
      document["event"] = "capture_failed";
      document["success"] = false;
      document["command_id"] = command.commandId;
      document["inspection_id"] = command.inspectionId;
      document["view"] = command.view;
      document["angle"] = command.angle;
      document["error"] = "camera_capture_failed";
      document["uptime_ms"] = millis();
      terminalPayload = serializeDocument(document);
    } else {
      Serial.print("Captured bytes: ");
      Serial.println(frame->len);

      int uploadStatus = -1;
      int uploadAttempts = 0;
      String uploadResponse;
      bool uploadSucceeded = false;
      unsigned long uploadStarted = millis();

      for (
        uploadAttempts = 1;
        uploadAttempts <= MAX_UPLOAD_ATTEMPTS;
        uploadAttempts++
      ) {
        uploadSucceeded = uploadJpegToPi(
          frame,
          command,
          uploadStatus,
          uploadResponse
        );

        if (uploadSucceeded) {
          break;
        }

        Serial.print("Upload attempt failed: ");
        Serial.print(uploadAttempts);
        Serial.print(" HTTP ");
        Serial.println(uploadStatus);
        Serial.println(uploadResponse);

        mqttClient.poll();
        delay(500);
      }

      unsigned long uploadMs = millis() - uploadStarted;
      size_t capturedBytes = frame->len;

      esp_camera_fb_return(frame);

      if (!uploadSucceeded) {
        failedCaptures++;

        JsonDocument document;
        document["device_id"] = DEVICE_ID;
        document["event"] = "capture_failed";
        document["success"] = false;
        document["command_id"] = command.commandId;
        document["inspection_id"] = command.inspectionId;
        document["view"] = command.view;
        document["angle"] = command.angle;
        document["error"] = "pi_upload_failed";
        document["http_status"] = uploadStatus;
        document["pi_response"] = uploadResponse;
        document["upload_attempts"] = uploadAttempts - 1;
        document["upload_ms"] = uploadMs;
        document["uptime_ms"] = millis();
        terminalPayload = serializeDocument(document);
      } else {
        successfulCaptures++;

        JsonDocument document;
        document["device_id"] = DEVICE_ID;
        document["event"] = "capture_complete";
        document["success"] = true;
        document["command_id"] = command.commandId;
        document["inspection_id"] = command.inspectionId;
        document["view"] = command.view;
        document["angle"] = command.angle;
        document["bytes"] = capturedBytes;
        document["http_status"] = uploadStatus;
        document["upload_attempts"] = uploadAttempts;
        document["upload_ms"] = uploadMs;
        document["uptime_ms"] = millis();
        terminalPayload = serializeDocument(document);
      }
    }
  }

  // Cache the terminal response before publishing it. If the MQTT link drops
  // during publication, the Pi can repeat the same command ID and retrieve it.
  lastCommandId = command.commandId;
  lastTerminalPayload = terminalPayload;

  captureBusy = false;
  activeCommandId = "";

  publishPayload(
    MQTT_EVENT_TOPIC,
    terminalPayload,
    1,
    false
  );

  publishStatus(true, "capture_finished");
}


// ============================================================
// HTTP debug routes
// ============================================================

void sendJson(int statusCode, const String &payload) {
  server.send(statusCode, "application/json", payload);
}


void handleRoot() {
  sendJson(
    200,
    "{"
      "\"device\":\"qc-camera-01\","
      "\"stage\":2,"
      "\"capture_commands\":\"mqtt\","
      "\"jpeg_upload\":\"http\","
      "\"routes\":[\"/status\",\"/capture\"]"
    "}"
  );
}


void handleStatus() {
  JsonDocument document;

  document["device"] = DEVICE_ID;
  document["stage"] = 2;
  document["camera_ready"] = cameraReady;
  document["capture_busy"] = captureBusy;
  document["queued_command"] = queuedCommand.pending;
  document["wifi_connected"] = WiFi.status() == WL_CONNECTED;
  document["mqtt_connected"] = mqttClient.connected();
  document["mqtt_broker"] = MQTT_HOST;
  document["successful_captures"] = successfulCaptures;
  document["failed_captures"] = failedCaptures;
  document["active_command_id"] = activeCommandId;
  document["last_command_id"] = lastCommandId;

  sendJson(200, serializeDocument(document));
}


void handleTestCapture() {
  if (!cameraReady) {
    server.send(503, "text/plain", "camera not ready");
    return;
  }

  if (captureBusy || queuedCommand.pending) {
    server.send(409, "text/plain", "camera busy");
    return;
  }

  captureBusy = true;

  camera_fb_t *frame = esp_camera_fb_get();

  if (!frame) {
    captureBusy = false;
    failedCaptures++;
    server.send(500, "text/plain", "capture failed");
    return;
  }

  server.setContentLength(frame->len);
  server.send(200, "image/jpeg", "");
  server.client().write(frame->buf, frame->len);

  esp_camera_fb_return(frame);

  successfulCaptures++;
  captureBusy = false;
}


// ============================================================
// Setup and main loop
// ============================================================

void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

  Serial.begin(115200);
  delay(1000);

  connectWiFi();

  Serial.print("Initializing camera... ");
  cameraReady = initializeCamera();
  Serial.println(cameraReady ? "ready" : "failed");

  mqttClient.setId(DEVICE_ID);
  mqttClient.setCleanSession(true);
  mqttClient.setKeepAliveInterval(30000);
  mqttClient.setConnectionTimeout(5000);
  mqttClient.setTxPayloadSize(1024);
  mqttClient.onMessage(onMqttMessage);
  configureMqttWill();

  server.on("/", HTTP_GET, handleRoot);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/capture", HTTP_GET, handleTestCapture);

  server.onNotFound(
    []() {
      sendJson(
        404,
        "{\"success\":false,\"error\":\"route_not_found\"}"
      );
    }
  );

  server.begin();
  Serial.println("ESP32-CAM HTTP debug server started");
  Serial.println("Stage 2 capture commands now arrive through MQTT");

  // Permit the first connection attempt immediately.
  lastMqttConnectAttemptMs = millis() - MQTT_RECONNECT_INTERVAL_MS;
  connectMqttIfNeeded();
}


void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    mqttClient.stop();
    connectWiFi();
  }

  connectMqttIfNeeded();

  if (mqttClient.connected()) {
    mqttClient.poll();

    unsigned long now = millis();

    if (now - lastHeartbeatMs >= HEARTBEAT_INTERVAL_MS) {
      lastHeartbeatMs = now;
      publishHeartbeat();
    }
  }

  server.handleClient();
  processQueuedCapture();
}
