#include <ESP8266WiFi.h>
#include <ESP8266WiFiMulti.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ESP8266WebServer.h>
#include <FS.h>
#include <time.h>

// Define NodeMCU pin names if not already defined
#ifndef D0
  #define D0 16
#endif
#ifndef D1
  #define D1 5
#endif
#ifndef D2
  #define D2 4
#endif
#ifndef D3
  #define D3 0
#endif
#ifndef D4
  #define D4 2
#endif
#ifndef D5
  #define D5 14
#endif
#ifndef D6
  #define D6 12
#endif
#ifndef D7
  #define D7 13
#endif
#ifndef D8
  #define D8 15
#endif

// Create WiFiMulti and WebServer objects
ESP8266WiFiMulti WiFiMulti;
ESP8266WebServer server(80);

// Global timing variables
unsigned long lastUpdateTime = 0;
unsigned long lastBlinkTime = 0;
// Auto restart variables (set to 6 hours here)
unsigned long lastRestartTime = 0;
const unsigned long restartInterval = 6UL * 60UL * 60UL * 1000UL; // 6 hours in ms

// Pin configuration for two ultrasonic sensors
#define TRIG_PIN_1  D5    // Example: D5 (GPIO14)
#define ECHO_PIN_1  D6    // Example: D6 (GPIO12)
#define TRIG_PIN_2  D7    // Example: D7 (GPIO13)
#define ECHO_PIN_2  D8    // Example: D8 (GPIO15)

// Tank level thresholds (modifiable via web)
int tank1_full_distance = 20;
int tank1_empty_distance = 150;
int tank2_full_distance = 20;
int tank2_empty_distance = 150;

// API update interval (in milliseconds; default 60000ms = 60s)
unsigned long apiUpdateInterval = 60000;

// Built-in LED pin (commonly D4 on many ESP8266 boards)
#ifndef LED_BUILTIN
  #define LED_BUILTIN D4
#endif

// WiFi credentials
const char* ssid = "KALPRAJ";
const char* password = "Kalpraj@123";

// API settings
const char* serverAddressApi = "water-level.kalprajsolutions.net";  // mDNS name
const char* apiUrl = "/api/water-levels";
const int portHttps = 443;  // HTTPS Port

// Log file size limits (in bytes)
const size_t MAX_LOG_SIZE = 5000;   // Maximum log file size
const size_t ROTATE_SIZE  = 2500;    // When rotating, keep last ROTATE_SIZE bytes

// ----------------------------------------------------------
// NTP Time Setup (IST: UTC+5:30)
// ----------------------------------------------------------
void setupTime() {
  // Set timezone offset of 19800 seconds (5.5 hours) and no DST
  configTime(19800, 0, "pool.ntp.org", "time.nist.gov");
  struct tm timeinfo;
  int retry = 0;
  while (!getLocalTime(&timeinfo) && retry < 10) {
    delay(1000);
    retry++;
  }
  if (retry >= 10) {
    Serial.println("Failed to obtain time");
  } else {
    Serial.println("Time synchronized");
  }
}

// ----------------------------------------------------------
// Logging Functions with Timestamp and Rotation
// ----------------------------------------------------------

// Rotate log if it exceeds MAX_LOG_SIZE
void rotateLogIfNeeded() {
  File f = SPIFFS.open("/log.txt", "r");
  if (!f) return;
  size_t fileSize = f.size();
  f.close();
  
  if (fileSize > MAX_LOG_SIZE) {
    f = SPIFFS.open("/log.txt", "r");
    String content = "";
    if (f) {
      content = f.readString();
      f.close();
    }
    if (content.length() > ROTATE_SIZE) {
      content = content.substring(content.length() - ROTATE_SIZE);
    }
    SPIFFS.remove("/log.txt");
    f = SPIFFS.open("/log.txt", "w");
    if (f) {
      f.print(content);
      f.close();
    }
  }
}

// Get current timestamp in IST formatted as [YYYY-MM-DD HH:MM:SS]
String getTimestamp() {
  struct tm timeinfo;
  if(getLocalTime(&timeinfo)){
    char buffer[30];
    strftime(buffer, sizeof(buffer), "[%Y-%m-%d %H:%M:%S] ", &timeinfo);
    return String(buffer);
  }
  return "[No Time] ";
}

// Log message to Serial and SPIFFS
void logMessage(String message) {
  String timestampedMsg = getTimestamp() + message;
  Serial.println(timestampedMsg);
  rotateLogIfNeeded();
  File f = SPIFFS.open("/log.txt", "a");
  if (f) {
    f.println(timestampedMsg);
    f.close();
  }
}

// ----------------------------------------------------------
// Sensor and API Functions
// ----------------------------------------------------------

// Function to calculate the mode of an array
int calculateMode(int readings[], int size) {
  int mode = readings[0];
  int maxCount = 0;
  for (int i = 0; i < size; i++) {
    int count = 0;
    for (int j = 0; j < size; j++) {
      if (readings[j] == readings[i]) {
        count++;
      }
    }
    if (count > maxCount) {
      maxCount = count;
      mode = readings[i];
    }
  }
  return mode;
}

// Measure distance using ultrasonic sensor
float getDistance(int trigPin, int echoPin) {
  digitalWrite(trigPin, LOW);
  delayMicroseconds(2);
  digitalWrite(trigPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(trigPin, LOW);
  long duration = pulseIn(echoPin, HIGH, 30000);
  if (duration == 0) return -1.0;  // Sensor error
  return (duration * 0.0343 / 2);   // Convert duration to cm
}

// Calculate tank level as a percentage using 5 readings and return the mode of valid readings
int getTankLevel(int trigPin, int echoPin, int fullDistance, int emptyDistance) {
  const int numSamples = 5;
  int validReadings[numSamples];
  int count = 0;

  for (int i = 0; i < numSamples; i++) {
    float distance = getDistance(trigPin, echoPin);
    if (distance != -1.0) {
      distance = constrain(distance, fullDistance, emptyDistance);
      validReadings[count++] = map(distance, emptyDistance, fullDistance, 0, 100);
    }
    delay(200); // Small delay between readings
  }
  if (count == 0) return -1;
  return calculateMode(validReadings, count);
}

// Send sensor data to remote API via HTTPS
void sendDataToApi(int tank1Level, int tank2Level) {
  if (WiFi.status() == WL_CONNECTED) {
    WiFiClientSecure client;
    client.setInsecure();  // For testing only (bypass certificate validation)
    HTTPClient http;
    String fullUrl = String("https://") + serverAddressApi + apiUrl;
    String contentType = "application/x-www-form-urlencoded";
    String postData = "tank1_level=" + String(tank1Level) + "&tank2_level=" + String(tank2Level);
    logMessage("Sending data: " + postData);
    if (http.begin(client, fullUrl)) {
      http.addHeader("Content-Type", contentType);
      int httpResponseCode = http.POST(postData);
      String response = http.getString();
      if (httpResponseCode < 200 || httpResponseCode >= 300) {
        logMessage("API call failed. Status Code: " + String(httpResponseCode) + ", Response: " + response);
      } else {
        logMessage("API call succeeded. Status Code: " + String(httpResponseCode));
      }
      http.end();
    } else {
      logMessage("[HTTP] Unable to connect");
    }
  } else {
    logMessage("WiFi not connected. Skipping API call.");
  }
}

// ----------------------------------------------------------
// Web Server Handlers
// ----------------------------------------------------------

// Root page: configuration and live readings (Bootstrap styled)
void handleRoot() {
  String html = "<!DOCTYPE html><html lang='en'><head>";
  html += "<meta charset='utf-8'><meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<link rel='stylesheet' href='https://maxcdn.bootstrapcdn.com/bootstrap/3.3.7/css/bootstrap.min.css'>";
  html += "<title>ESP8266 Water Monitor</title></head><body>";
  html += "<div class='container'><h1 class='text-center'>ESP8266 Water Monitor</h1>";
  
  // Live readings panel
  html += "<div class='panel panel-info'><div class='panel-heading'><h3 class='panel-title'>Live Tank Readings</h3></div>";
  html += "<div class='panel-body'><p>Tank 1 Level: <span id='tank1'>--</span></p>";
  html += "<p>Tank 2 Level: <span id='tank2'>--</span></p></div></div>";
  
  // Configuration form panel
  html += "<div class='panel panel-default'><div class='panel-heading'><h3 class='panel-title'>Configuration</h3></div>";
  html += "<div class='panel-body'>";
  html += "<form action='/update' method='GET' class='form-horizontal'>";
  
  html += "<div class='form-group'><label class='col-sm-4 control-label'>Tank1 Full Distance (cm):</label>";
  html += "<div class='col-sm-8'><input type='number' class='form-control' name='t1full' value='" + String(tank1_full_distance) + "'></div></div>";
  
  html += "<div class='form-group'><label class='col-sm-4 control-label'>Tank1 Empty Distance (cm):</label>";
  html += "<div class='col-sm-8'><input type='number' class='form-control' name='t1empty' value='" + String(tank1_empty_distance) + "'></div></div>";
  
  html += "<div class='form-group'><label class='col-sm-4 control-label'>Tank2 Full Distance (cm):</label>";
  html += "<div class='col-sm-8'><input type='number' class='form-control' name='t2full' value='" + String(tank2_full_distance) + "'></div></div>";
  
  html += "<div class='form-group'><label class='col-sm-4 control-label'>Tank2 Empty Distance (cm):</label>";
  html += "<div class='col-sm-8'><input type='number' class='form-control' name='t2empty' value='" + String(tank2_empty_distance) + "'></div></div>";
  
  html += "<div class='form-group'><label class='col-sm-4 control-label'>API Update Interval (ms):</label>";
  html += "<div class='col-sm-8'><input type='number' class='form-control' name='interval' value='" + String(apiUpdateInterval) + "'></div></div>";
  
  html += "<div class='form-group'><div class='col-sm-offset-4 col-sm-8'>";
  html += "<button type='submit' class='btn btn-primary'>Update Settings</button>";
  html += "</div></div></form></div></div>";
  
  html += "<a href='/log' class='btn btn-default'>View Log File</a> ";
  html += "<a href='/wifi' class='btn btn-info'>WiFi Status</a></div>";
  
  // JavaScript for live readings update via AJAX every API interval
  html += "<script src='https://ajax.googleapis.com/ajax/libs/jquery/3.2.1/jquery.min.js'></script>";
  html += "<script>function fetchReadings() { $.getJSON('/readings', function(data) { $('#tank1').text(data.tank1 + '%'); $('#tank2').text(data.tank2 + '%'); }); }";
  html += "setInterval(fetchReadings, "+String(apiUpdateInterval)+"); fetchReadings();</script>";
  
  html += "</body></html>";
  server.send(200, "text/html", html);
}

// Update settings handler
void handleUpdate() {
  if (server.hasArg("t1full")) tank1_full_distance = server.arg("t1full").toInt();
  if (server.hasArg("t1empty")) tank1_empty_distance = server.arg("t1empty").toInt();
  if (server.hasArg("t2full")) tank2_full_distance = server.arg("t2full").toInt();
  if (server.hasArg("t2empty")) tank2_empty_distance = server.arg("t2empty").toInt();
  if (server.hasArg("interval")) apiUpdateInterval = server.arg("interval").toInt();
  
  logMessage("Updated settings: Tank1 Full: " + String(tank1_full_distance) +
             ", Tank1 Empty: " + String(tank1_empty_distance) +
             ", Tank2 Full: " + String(tank2_full_distance) +
             ", Tank2 Empty: " + String(tank2_empty_distance) +
             ", Update Interval: " + String(apiUpdateInterval) + "ms");
  
  server.sendHeader("Location", "/");
  server.send(303);
}

// Log file handler
void handleLog() {
  String logContent = "";
  File f = SPIFFS.open("/log.txt", "r");
  if (f) {
    while (f.available()) {
      logContent += char(f.read());
    }
    f.close();
  } else {
    logContent = "Log file not found.";
  }
  String html = "<!DOCTYPE html><html lang='en'><head>";
  html += "<meta charset='utf-8'><meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<link rel='stylesheet' href='https://maxcdn.bootstrapcdn.com/bootstrap/3.3.7/css/bootstrap.min.css'>";
  html += "<title>ESP8266 Log</title></head><body>";
  html += "<div class='container'><h1>Log File</h1><pre>" + logContent + "</pre>";
  html += "<a href='/' class='btn btn-default'>Back</a></div></body></html>";
  server.send(200, "text/html", html);
}

// Readings handler: returns JSON with current sensor readings
void handleReadings() {
  int t1 = getTankLevel(TRIG_PIN_1, ECHO_PIN_1, tank1_full_distance, tank1_empty_distance);
  int t2 = getTankLevel(TRIG_PIN_2, ECHO_PIN_2, tank2_full_distance, tank2_empty_distance);
  String json = "{\"tank1\":" + String(t1) + ",\"tank2\":" + String(t2) + "}";
  server.send(200, "application/json", json);
}

// ----------------------------------------------------------
// WiFi Status and Reconnect Handlers
// ----------------------------------------------------------
void handleWifi() {
  String html = "<!DOCTYPE html><html lang='en'><head>";
  html += "<meta charset='utf-8'><meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<link rel='stylesheet' href='https://maxcdn.bootstrapcdn.com/bootstrap/3.3.7/css/bootstrap.min.css'>";
  html += "<title>WiFi Status</title></head><body>";
  html += "<div class='container'><h1>WiFi Status</h1>";
  
  // Check WiFi connection status
  String status = (WiFi.status() == WL_CONNECTED) ? "Connected" : "Disconnected";
  html += "<p><strong>WiFi Connection:</strong> " + status + "</p>";
  
  // Check Internet connectivity via DNS lookup
  IPAddress remoteIP;
  bool internet = WiFi.hostByName("google.com", remoteIP);
  html += "<p><strong>Internet Connectivity:</strong> " + String(internet ? "Available" : "Not Available") + "</p>";
  
  html += "<form action='/forceWifi' method='GET'><button type='submit' class='btn btn-warning'>Force WiFi Reconnect</button></form>";
  html += "<br><a href='/' class='btn btn-default'>Back</a></div></body></html>";
  server.send(200, "text/html", html);
}

// Handler to force WiFi reconnect
void handleForceWifi() {
  logMessage("Forcing WiFi reconnect as per user request.");
  WiFi.disconnect();
  delay(500);
  WiFiMulti.addAP(ssid, password);
  // Wait briefly for reconnection
  unsigned long start = millis();
  while(WiFiMulti.run() != WL_CONNECTED && millis() - start < 5000) {
    delay(200);
  }
  server.sendHeader("Location", "/wifi");
  server.send(303);
}

// ----------------------------------------------------------
// Setup and Loop
// ----------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n\nStarting ESP8266 Water Level Monitor...\n");
  
  // Initialize SPIFFS
  if (!SPIFFS.begin()) {
    Serial.println("SPIFFS Mount Failed");
  }
  
  // Log restart event
  logMessage("=== ESP Restarted ===");
  
  WiFi.mode(WIFI_STA);
  WiFiMulti.addAP(ssid, password);
  
  Serial.print("Connecting to WiFi");
  while (WiFiMulti.run() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected.");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());
  
  setupTime();
  
  // Setup web server routes
  server.on("/", handleRoot);
  server.on("/update", handleUpdate);
  server.on("/log", handleLog);
  server.on("/readings", handleReadings);
  server.on("/wifi", handleWifi);
  server.on("/forceWifi", handleForceWifi);
  server.begin();
  Serial.println("Web server started.");
  
  // Sensor pin setup
  pinMode(TRIG_PIN_1, OUTPUT);
  pinMode(ECHO_PIN_1, INPUT);
  pinMode(TRIG_PIN_2, OUTPUT);
  pinMode(ECHO_PIN_2, INPUT);
  
  // LED setup
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);
  
  // Initialize restart timer
  lastRestartTime = millis();
}

void loop() {
  unsigned long currentMillis = millis();
  
  server.handleClient();
  
  // WiFi reconnection handling
  if (WiFiMulti.run() != WL_CONNECTED) {
    logMessage("WiFi disconnected. Attempting to reconnect...");
    delay(500);
  }
  
  // API update block
  if (currentMillis - lastUpdateTime >= apiUpdateInterval) {
    int t1 = getTankLevel(TRIG_PIN_1, ECHO_PIN_1, tank1_full_distance, tank1_empty_distance);
    int t2 = getTankLevel(TRIG_PIN_2, ECHO_PIN_2, tank2_full_distance, tank2_empty_distance);
    
    logMessage("Tank 1 Level: " + String(t1));
    logMessage("Tank 2 Level: " + String(t2));
    
    sendDataToApi(t1, t2);
    lastUpdateTime = currentMillis;
  }
  
  // LED blink for status
  if (currentMillis - lastBlinkTime >= 2000) {
    digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
    lastBlinkTime = currentMillis;
  }
  
  // Auto-restart if restart interval exceeded
  if (currentMillis - lastRestartTime >= restartInterval) {
    logMessage("Restart interval reached. Restarting ESP...");
    delay(100);  // Allow logs to flush
    ESP.restart();
  }
}
