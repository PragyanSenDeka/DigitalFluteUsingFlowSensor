/*
 * Digital Flute - ESP32-C6 Firmware
 * ===================================
 * Hardware: ESP32-C6 + YF-S201 Flow Sensor on GPIO3
 *
 * Libraries needed (install via Arduino Library Manager — search by name):
 *   1. "WebSockets"  by Markus Sattler
 *   2. "ArduinoJson" by Benoit Blanchon
 *
 * The built-in WiFi.h and WebServer.h need no installation.
 *
 * Wiring:
 *   YF-S201 VCC → 3.3V
 *   YF-S201 GND → GND
 *   YF-S201 SIG → GPIO3
 */

#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <ArduinoJson.h>

// ── WiFi credentials ──────────────────────────────────────────────────────────
const char* WIFI_SSID     = "TBE3162";      // <- change me
const char* WIFI_PASSWORD = "f38U175!";  // <- change me

// ── Pin & sensor config ───────────────────────────────────────────────────────
#define FLOW_SENSOR_PIN      3      // GPIO3
#define SAMPLE_INTERVAL      80     // ms between WebSocket broadcasts
#define PULSE_TIMEOUT        2000   // ms silence -> treat as zero flow
#define PULSES_PER_LITER_MIN 7.5f   // YF-S201 datasheet (some units: 5.5 or 6.6)
#define FLOW_THRESHOLD       0.15f  // L/min minimum to count as "blowing"
#define FLOW_MAX             8.0f   // L/min at full breath (for normalisation)

// ── Servers ───────────────────────────────────────────────────────────────────
WebServer        httpServer(80);
WebSocketsServer wsServer(81);     // WebSocket on port 81

// ── Flow sensor state ─────────────────────────────────────────────────────────
volatile uint32_t pulseCount  = 0;
uint32_t lastSampleTime       = 0;
uint32_t lastPulseTime        = 0;
float    currentFlowRate      = 0.0f;

// ── ISR ───────────────────────────────────────────────────────────────────────
void IRAM_ATTR onFlowPulse() {
  pulseCount++;
  lastPulseTime = millis();
}

// ── WebSocket events ──────────────────────────────────────────────────────────
void onWsEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
  if (type == WStype_CONNECTED) {
    Serial.printf("[WS] Client %u connected\n", num);
  } else if (type == WStype_DISCONNECTED) {
    Serial.printf("[WS] Client %u disconnected\n", num);
  }
}

// ── Broadcast flow data to all WS clients ────────────────────────────────────
void broadcastFlowData() {
  float norm = 0.0f;
  if (currentFlowRate >= FLOW_THRESHOLD) {
    norm = constrain(
      (currentFlowRate - FLOW_THRESHOLD) / (FLOW_MAX - FLOW_THRESHOLD),
      0.0f, 1.0f
    );
  }

  StaticJsonDocument<128> doc;
  doc["flow"]    = currentFlowRate;
  doc["breath"]  = norm;
  doc["blowing"] = (currentFlowRate >= FLOW_THRESHOLD);

  char buf[128];
  serializeJson(doc, buf);
  wsServer.broadcastTXT(buf);
}

// ── Minimal landing page served by HTTP ──────────────────────────────────────
void handleRoot() {
  String ip = (WiFi.status() == WL_CONNECTED)
              ? WiFi.localIP().toString()
              : WiFi.softAPIP().toString();

  String html =
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<title>Digital Flute</title>"
    "<style>body{background:#0a0a0f;color:#fff;font-family:monospace;"
    "display:flex;flex-direction:column;align-items:center;"
    "justify-content:center;height:100vh;margin:0;gap:16px;}"
    "h2{color:#7df9aa;letter-spacing:4px;}"
    "p{color:#888;text-align:center;max-width:480px;line-height:1.7;}"
    "code{background:#1a1a2e;padding:3px 8px;border-radius:4px;color:#3af4f4;}"
    "</style></head><body>"
    "<h2>DIGITAL FLUTE</h2>"
    "<p>ESP32 is running!</p>"
    "<p>Open <strong>index.html</strong> on your computer and set the "
    "WebSocket URL to:<br><br>"
    "<code>ws://" + ip + ":81</code></p>"
    "<p>Then click <strong>Connect</strong>.</p>"
    "</body></html>";

  httpServer.send(200, "text/html", html);
}

// ── setup ─────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println("\n=== Digital Flute Booting ===");

  // Flow sensor
  pinMode(FLOW_SENSOR_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(FLOW_SENSOR_PIN), onFlowPulse, RISING);

  // WiFi
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.printf("Connecting to %s", WIFI_SSID);

  uint8_t tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 30) {
    delay(500);
    Serial.print(".");
    tries++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\nConnected!\nOpen:          http://%s\nWebSocket URL: ws://%s:81\n",
                  WiFi.localIP().toString().c_str(),
                  WiFi.localIP().toString().c_str());
  } else {
    // Fall back to Access Point mode
    WiFi.mode(WIFI_AP);
    WiFi.softAP("DigitalFlute", "flute1234");
    Serial.printf("\nAP mode! Connect phone/laptop to WiFi 'DigitalFlute' (pass: flute1234)\n");
    Serial.printf("Open:          http://%s\nWebSocket URL: ws://%s:81\n",
                  WiFi.softAPIP().toString().c_str(),
                  WiFi.softAPIP().toString().c_str());
  }

  // HTTP server
  httpServer.on("/", handleRoot);
  httpServer.begin();
  Serial.println("HTTP server started on port 80");

  // WebSocket server
  wsServer.begin();
  wsServer.onEvent(onWsEvent);
  Serial.println("WebSocket server started on port 81");

  lastSampleTime = millis();
}

// ── loop ──────────────────────────────────────────────────────────────────────
void loop() {
  httpServer.handleClient();   // handle HTTP requests
  wsServer.loop();             // handle WebSocket traffic

  uint32_t now = millis();

  if (now - lastSampleTime >= SAMPLE_INTERVAL) {
    uint32_t elapsed = now - lastSampleTime;
    lastSampleTime   = now;

    // Safely read and reset pulse counter
    noInterrupts();
    uint32_t pulses = pulseCount;
    pulseCount      = 0;
    interrupts();

    // No pulse for a while -> sensor idle
    if (now - lastPulseTime > PULSE_TIMEOUT) pulses = 0;

    // Convert to L/min
    float pulsesPerSec = (float)pulses / ((float)elapsed / 1000.0f);
    currentFlowRate    = pulsesPerSec / PULSES_PER_LITER_MIN;
    if (currentFlowRate < 0.05f) currentFlowRate = 0.0f;

    broadcastFlowData();
  }
}
