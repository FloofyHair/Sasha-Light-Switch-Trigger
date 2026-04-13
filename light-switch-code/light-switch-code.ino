#include <WiFi.h>
#include <HTTPClient.h>
#include <time.h>

#include "credentials.h"  // must define WIFI_SSID and WIFI_PASS

#ifndef WIFI_SSID
#error "WIFI_SSID is not defined. Copy credentials.example.h to credentials.h and fill in your SSID."
#endif
#ifndef WIFI_PASS
#error "WIFI_PASS is not defined. Copy credentials.example.h to credentials.h and fill in your password."
#endif

const char* ssid = WIFI_SSID;
const char* password = WIFI_PASS;

// REST endpoints served by the Flask app (see app.py)
const char* url_check   = "https://sasha-light-switch-trigger.onrender.com/check";   // GET -> {"trigger": bool}, resets flag when true
const char* url_state   = "https://sasha-light-switch-trigger.onrender.com/api/state"; // GET -> includes alarm_time

// Pick your output pin
const int pin = 10;   // safer than GPIO 10 on most ESP32 boards

// Local alarm fallback (24h)
int alarmHour = 7;
int alarmMinute = 0;
bool firedToday = false;
const char* tzInfo = "EST5EDT,M3.2.0/2,M11.1.0/2";
int lastFireYDay = -1;

unsigned long lastStateFetchMs = 0;
const unsigned long STATE_FETCH_INTERVAL_MS = 60UL * 1000UL;

// Fallback for boards missing Arduino's getLocalTime helper
bool getLocalTimeSafe(struct tm* info, uint32_t ms = 5000) {
  time_t now;
  uint32_t start = millis();
  while ((millis() - start) <= ms) {
    time(&now);
    if (now > 1600000000) { // after 2020 -> time is valid
      localtime_r(&now, info);
      return true;
    }
    delay(10);
  }
  return false;
}

void setupTime() {
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  setenv("TZ", tzInfo, 1);
  tzset();

  Serial.print("Waiting for time");
  struct tm timeinfo;
  while (!getLocalTimeSafe(&timeinfo)) {
    Serial.print(".");
    delay(500);
  }
  Serial.println();
  Serial.println("Time synced");
}

bool parseAlarm(const String& alarmStr) {
  int h, m;
  if (sscanf(alarmStr.c_str(), "%d:%d", &h, &m) == 2) {
    if (h >= 0 && h < 24 && m >= 0 && m < 60) {
      alarmHour = h;
      alarmMinute = m;
      return true;
    }
  }
  return false;
}

void pulseOutput() {
  Serial.println("Trigger HIGH");
  digitalWrite(pin, HIGH);
  delay(1000);
  digitalWrite(pin, LOW);
}

void ensureWifi() {
  if (WiFi.status() == WL_CONNECTED) return;

  WiFi.disconnect();
  WiFi.begin(ssid, password);
  Serial.print("Reconnecting WiFi");
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 30) {
    delay(500);
    Serial.print(".");
    tries++;
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Reconnected. IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("WiFi reconnect failed");
  }
}

bool checkHttpTrigger() {
  ensureWifi();
  if (WiFi.status() != WL_CONNECTED) return false;

  HTTPClient http;
  http.begin(url_check);

  int httpCode = http.GET();

  if (httpCode > 0) {
    String payload = http.getString();
    Serial.printf("HTTP %d: %s\n", httpCode, payload.c_str());

    // naive JSON check for `"trigger":true`
    if (payload.indexOf("\"trigger\":true") != -1 || payload.indexOf("true") != -1) {
      pulseOutput();
    }
    http.end();
    return true;
  } else {
    Serial.print("HTTP error: ");
    Serial.println(httpCode);
  }

  http.end();
  return false;
}

bool fetchAlarmFromServer() {
  ensureWifi();
  if (WiFi.status() != WL_CONNECTED) return false;

  HTTPClient http;
  http.begin(url_state);
  int httpCode = http.GET();
  if (httpCode <= 0) {
    http.end();
    return false;
  }
  String payload = http.getString();
  http.end();

  int idx = payload.indexOf("\"alarm_time\":\"");
  if (idx == -1) return false;
  idx += 14;
  int end = payload.indexOf("\"", idx);
  if (end == -1) return false;

  String alarmStr = payload.substring(idx, end);
  if (parseAlarm(alarmStr)) {
    Serial.print("Updated alarm from server: ");
    Serial.println(alarmStr);
    return true;
  }
  return false;
}

void checkLocalAlarm() {
  struct tm timeinfo;
  if (!getLocalTimeSafe(&timeinfo)) return;

  // reset daily flag when date changes
  if (lastFireYDay != timeinfo.tm_yday) {
    firedToday = false;
  }

  if (timeinfo.tm_hour == alarmHour && timeinfo.tm_min == alarmMinute && timeinfo.tm_sec <= 2) {
    if (!firedToday) {
      Serial.println("Local alarm fired");
      pulseOutput();
      firedToday = true;
      lastFireYDay = timeinfo.tm_yday;
    }
  }
}

void setup() {
  Serial.begin(115200);

  pinMode(pin, OUTPUT);
  digitalWrite(pin, LOW);

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println();
  Serial.println("Connected");
  Serial.println(WiFi.localIP());

  setupTime();
}

void loop() {
  checkHttpTrigger(); // remote trigger if server reachable

  unsigned long nowMs = millis();
  if (nowMs - lastStateFetchMs > STATE_FETCH_INTERVAL_MS) {
    if (fetchAlarmFromServer()) {
      lastStateFetchMs = nowMs;
    }
  }

  checkLocalAlarm(); // always check local schedule

  delay(1000);
}
