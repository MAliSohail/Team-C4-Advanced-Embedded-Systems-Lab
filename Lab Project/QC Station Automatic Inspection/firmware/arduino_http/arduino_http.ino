/*
 * QC Station — Arduino Stage 1
 *
 * Hardware:
 *   Arduino Uno WiFi Rev2
 *   Button between pin 2 and GND
 *   Positional servo signal on pin 9
 *
 * Responsibilities:
 *   1. Detect the operator button press.
 *   2. Notify the Raspberry Pi using:
 *        POST /api/inspection/start
 *   3. Accept commands from the Raspberry Pi:
 *        GET /move?angle=0
 *        GET /move?angle=60
 *        GET /move?angle=120
 *        GET /move?angle=180
 *        GET /home
 *        GET /status
 *
 * Libraries:
 *   WiFiNINA
 *   Servo
 */

#include <WiFiNINA.h>
#include <Servo.h>
#include "secrets.h"


// ============================================================
// Pins and mechanical configuration
// ============================================================

constexpr uint8_t BUTTON_PIN = 2;
constexpr uint8_t SERVO_PIN = 9;

constexpr int MIN_ANGLE = 0;
constexpr int MAX_ANGLE = 180;

// Time given to the servo to physically reach the requested angle.
constexpr unsigned long SERVO_SETTLE_MS = 750;

// Return-home movement may travel the complete 180-degree range.
constexpr unsigned long HOME_SETTLE_MS = 1200;

// Button debounce time.
constexpr unsigned long DEBOUNCE_MS = 50;


// ============================================================
// Global objects and state
// ============================================================

WiFiServer server(80);
Servo turntableServo;

int currentAngle = 0;
bool stationBusy = false;
bool lastButtonState = HIGH;


// ============================================================
// Helper functions
// ============================================================

void printNetworkStatus() {
  Serial.print("Arduino IP: ");
  Serial.println(WiFi.localIP());

  Serial.print("Signal strength: ");
  Serial.print(WiFi.RSSI());
  Serial.println(" dBm");
}


void connectWiFi() {
  Serial.print("Connecting to WiFi");
  Serial.print("Trying SSID: [");
  Serial.print(WIFI_SSID);
  Serial.println("]");

  while (WiFi.status() != WL_CONNECTED) {
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.print(".");
    delay(2000);
  }

  Serial.println();
  Serial.println("WiFi connected");
  printNetworkStatus();
}


void sendJsonResponse(
  WiFiClient &client,
  int statusCode,
  const char *statusText,
  const String &json
) {
  client.print("HTTP/1.1 ");
  client.print(statusCode);
  client.print(" ");
  client.println(statusText);

  client.println("Content-Type: application/json");
  client.println("Connection: close");
  client.print("Content-Length: ");
  client.println(json.length());
  client.println();
  client.print(json);
}


String statusJson() {
  String json = "{";
  json += "\"device\":\"qc-turntable\",";
  json += "\"angle\":";
  json += String(currentAngle);
  json += ",";
  json += "\"busy\":";
  json += stationBusy ? "true" : "false";
  json += ",";
  json += "\"wifi_connected\":";
  json += WiFi.status() == WL_CONNECTED ? "true" : "false";
  json += "}";

  return json;
}


bool isAllowedAngle(int angle) {
  return (
    angle == 0 ||
    angle == 60 ||
    angle == 120 ||
    angle == 180
  );
}


void moveServoTo(int requestedAngle) {
  requestedAngle = constrain(
    requestedAngle,
    MIN_ANGLE,
    MAX_ANGLE
  );

  stationBusy = true;

  Serial.print("Moving servo from ");
  Serial.print(currentAngle);
  Serial.print(" to ");
  Serial.println(requestedAngle);

  turntableServo.write(requestedAngle);
  delay(SERVO_SETTLE_MS);

  currentAngle = requestedAngle;

  Serial.print("Position ready: ");
  Serial.println(currentAngle);
}


void moveServoHome() {
  stationBusy = true;

  Serial.print("Returning servo home from ");
  Serial.println(currentAngle);

  turntableServo.write(0);
  delay(HOME_SETTLE_MS);

  currentAngle = 0;
  stationBusy = false;

  Serial.println("Turntable home; station unlocked");
}


// ============================================================
// HTTP request parsing
// ============================================================

int readAngleParameter(const String &requestLine) {
  int parameterStart = requestLine.indexOf("angle=");

  if (parameterStart < 0) {
    return -1;
  }

  parameterStart += 6;

  int parameterEnd = requestLine.indexOf(' ', parameterStart);

  if (parameterEnd < 0) {
    parameterEnd = requestLine.length();
  }

  String value = requestLine.substring(
    parameterStart,
    parameterEnd
  );

  // Stop at another query parameter, if one exists.
  int ampersand = value.indexOf('&');

  if (ampersand >= 0) {
    value = value.substring(0, ampersand);
  }

  return value.toInt();
}


void consumeRemainingHeaders(WiFiClient &client) {
  unsigned long started = millis();

  while (
    client.connected() &&
    millis() - started < 1000
  ) {
    if (!client.available()) {
      continue;
    }

    String line = client.readStringUntil('\n');

    // Blank line means end of HTTP headers.
    if (line == "\r" || line.length() == 0) {
      break;
    }
  }
}


// ============================================================
// HTTP server
// ============================================================

void handleIncomingHttp() {
  WiFiClient client = server.available();

  if (!client) {
    return;
  }

  client.setTimeout(1000);

  String requestLine = client.readStringUntil('\r');
  client.readStringUntil('\n');

  consumeRemainingHeaders(client);

  Serial.print("HTTP request: ");
  Serial.println(requestLine);

  if (requestLine.startsWith("GET /move?")) {
    int requestedAngle = readAngleParameter(requestLine);

    if (!isAllowedAngle(requestedAngle)) {
      sendJsonResponse(
        client,
        400,
        "Bad Request",
        "{\"success\":false,"
        "\"error\":\"allowed angles are 0, 60, 120 and 180\"}"
      );
    } else {
      moveServoTo(requestedAngle);

      String response = "{";
      response += "\"success\":true,";
      response += "\"state\":\"position_ready\",";
      response += "\"angle\":";
      response += String(currentAngle);
      response += "}";

      sendJsonResponse(
        client,
        200,
        "OK",
        response
      );
    }
  }
  else if (requestLine.startsWith("GET /home")) {
    moveServoHome();

    sendJsonResponse(
      client,
      200,
      "OK",
      "{\"success\":true,"
      "\"state\":\"home_ready\","
      "\"angle\":0}"
    );
  }
  else if (requestLine.startsWith("GET /status")) {
    sendJsonResponse(
      client,
      200,
      "OK",
      statusJson()
    );
  }
  else if (requestLine.startsWith("GET / ")) {
    sendJsonResponse(
      client,
      200,
      "OK",
      statusJson()
    );
  }
  else {
    sendJsonResponse(
      client,
      404,
      "Not Found",
      "{\"success\":false,"
      "\"error\":\"route_not_found\"}"
    );
  }

  delay(10);
  client.stop();
}


// ============================================================
// Pi notification
// ============================================================

bool notifyPiItemPlaced() {
  WiFiClient client;

  Serial.println("Button pressed; notifying Raspberry Pi");

  if (!client.connect(PI_HOST, PI_PORT)) {
    Serial.println("Could not connect to Raspberry Pi");
    return false;
  }

  String body = "{}";

  client.println("POST /api/inspection/start HTTP/1.1");
  client.print("Host: ");
  client.print(PI_HOST);
  client.print(":");
  client.println(PI_PORT);

  client.println("Content-Type: application/json");
  client.println("X-Event-Source: arduino-button");
  client.println("Connection: close");
  client.print("Content-Length: ");
  client.println(body.length());
  client.println();
  client.print(body);

  unsigned long started = millis();
  bool accepted = false;
  int statusCode = 0;

  while (
    client.connected() &&
    millis() - started < 3000
  ) {
    if (!client.available()) {
      continue;
    }

    String statusLine = client.readStringUntil('\n');
    statusLine.trim();

    Serial.print("Pi response: ");
    Serial.println(statusLine);

    // Example:
    // HTTP/1.1 202 ACCEPTED
    int firstSpace = statusLine.indexOf(' ');
    int secondSpace = statusLine.indexOf(' ', firstSpace + 1);

    if (firstSpace >= 0 && secondSpace > firstSpace) {
      statusCode = statusLine.substring(
        firstSpace + 1,
        secondSpace
      ).toInt();
    }

    accepted = statusCode == 202;

    // Consume the rest of the response.
    while (
      client.connected() &&
      millis() - started < 3000
    ) {
      while (client.available()) {
        client.read();
      }
    }

    break;
  }

  client.stop();

  if (accepted) {
    stationBusy = true;
    Serial.println("Inspection accepted by Pi");
    return true;
  }

  if (statusCode == 409) {
    Serial.println("Pi reports station already busy");
  } else {
    Serial.print("Pi did not accept inspection; HTTP ");
    Serial.println(statusCode);
  }

  return false;
}


// ============================================================
// Button handling
// ============================================================

void handleButton() {
  bool buttonState = digitalRead(BUTTON_PIN);

  // Detect the transition from released to pressed.
  if (
    lastButtonState == HIGH &&
    buttonState == LOW
  ) {
    delay(DEBOUNCE_MS);

    if (digitalRead(BUTTON_PIN) == LOW) {
      if (stationBusy) {
        Serial.println(
          "Button ignored because inspection is already running"
        );
      } else {
        bool notified = notifyPiItemPlaced();

        if (!notified) {
          stationBusy = false;
        }
      }
    }
  }

  lastButtonState = buttonState;
}


// ============================================================
// Arduino setup and loop
// ============================================================

void setup() {
  Serial.begin(115200);

  pinMode(BUTTON_PIN, INPUT_PULLUP);

  turntableServo.attach(SERVO_PIN);
  turntableServo.write(0);

  currentAngle = 0;
  stationBusy = false;

  connectWiFi();

  server.begin();

  Serial.println("Arduino QC turntable ready");
  Serial.println("Routes:");
  Serial.println("  GET /status");
  Serial.println("  GET /move?angle=0");
  Serial.println("  GET /move?angle=60");
  Serial.println("  GET /move?angle=120");
  Serial.println("  GET /move?angle=180");
  Serial.println("  GET /home");
}


void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
  }

  handleIncomingHttp();
  handleButton();
}