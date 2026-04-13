#include <WiFi.h>
#include <HTTPClient.h>
#include <time.h>

#include "credentials.h"  // must define WIFI_SSID and WIFI_PASS

#ifndef WIFI_SSID
#error "WIFI_SSID not defined — copy credentials.example.h to credentials.h and fill it in."
#endif
#ifndef WIFI_PASS
#error "WIFI_PASS not defined — copy credentials.example.h to credentials.h and fill it in."
#endif

// ── Configuration ─────────────────────────────────────────────────────────────
const char* CHECKIN_URL = "https://sasha-light-switch-trigger.onrender.com/checkin";
const int   OUTPUT_PIN  = 10;
const char* TZ_INFO     = "EST5EDT,M3.2.0/2,M11.1.0/2";

// How often the device checks in with the server (reports state / receives commands)
const unsigned long CHECKIN_INTERVAL_MS = 10UL * 1000UL;   // 10 seconds
// ──────────────────────────────────────────────────────────────────────────────

const char* ssid     = WIFI_SSID;
const char* password = WIFI_PASS;

// Local alarm — device is the source of truth for when to fire
int  alarmHour    = 7;
int  alarmMinute  = 0;
bool firedToday   = false;
int  lastFireYDay = -1;

// Trigger telemetry reported to server on each check-in
int    triggerCount   = 0;
String lastTriggerISO = "";

unsigned long lastCheckinMs = 0;


// ── Time helpers ──────────────────────────────────────────────────────────────

bool getLocalTimeSafe(struct tm* info, uint32_t timeoutMs = 5000) {
  time_t now;
  uint32_t start = millis();
  while ((millis() - start) <= timeoutMs) {
    time(&now);
    if (now > 1700000000UL) {   // after ~Nov 2023 — NTP has synced
      localtime_r(&now, info);
      return true;
    }
    delay(10);
  }
  return false;
}

void setupTime() {
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  setenv("TZ", TZ_INFO, 1);
  tzset();
  Serial.print("Syncing time");
  struct tm t;
  while (!getLocalTimeSafe(&t)) {
    Serial.print(".");
    delay(500);
  }
  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &t);
  Serial.printf("\nTime synced: %s\n", buf);
}


// ── WiFi ──────────────────────────────────────────────────────────────────────

void ensureWifi() {
  if (WiFi.status() == WL_CONNECTED) return;
  WiFi.disconnect();
  WiFi.begin(ssid, password);
  Serial.print("Reconnecting WiFi");
  for (int i = 0; i < 30 && WiFi.status() != WL_CONNECTED; i++) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED)
    Serial.printf("Reconnected. IP: %s\n", WiFi.localIP().toString().c_str());
  else
    Serial.println("WiFi reconnect failed");
}


// ── Output ────────────────────────────────────────────────────────────────────

void pulseOutput() {
  Serial.println("Trigger HIGH");
  digitalWrite(OUTPUT_PIN, HIGH);
  delay(1000);
  digitalWrite(OUTPUT_PIN, LOW);

  triggerCount++;
  struct tm t;
  if (getLocalTimeSafe(&t, 100)) {
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &t);
    lastTriggerISO = String(buf);
  }
  Serial.printf("Total triggers: %d\n", triggerCount);
}


// ── Local alarm ───────────────────────────────────────────────────────────────

bool parseAlarm(const String& s) {
  int h, m;
  if (sscanf(s.c_str(), "%d:%d", &h, &m) == 2 && h >= 0 && h < 24 && m >= 0 && m < 60) {
    alarmHour   = h;
    alarmMinute = m;
    return true;
  }
  return false;
}

void checkLocalAlarm() {
  struct tm t;
  if (!getLocalTimeSafe(&t, 100)) return;   // short timeout — don't block the loop

  if (lastFireYDay != t.tm_yday) {
    firedToday   = false;       // new day: reset
    lastFireYDay = t.tm_yday;
  }

  if (!firedToday && t.tm_hour == alarmHour && t.tm_min == alarmMinute && t.tm_sec <= 2) {
    Serial.println("Local alarm fired");
    firedToday = true;
    pulseOutput();
  }
}


// ── Server check-in ───────────────────────────────────────────────────────────

void doCheckin() {
  ensureWifi();
  if (WiFi.status() != WL_CONNECTED) return;

  // Build JSON payload with current device state
  char alarmStr[6];
  sprintf(alarmStr, "%02d:%02d", alarmHour, alarmMinute);

  String body = "{\"alarm_time\":\"";
  body += alarmStr;
  body += "\",\"trigger_count\":";
  body += triggerCount;
  if (lastTriggerISO.length() > 0) {
    body += ",\"last_trigger_iso\":\"";
    body += lastTriggerISO;
    body += "\"";
  }
  body += "}";

  HTTPClient http;
  http.begin(CHECKIN_URL);
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(body);

  if (code > 0) {
    String resp = http.getString();
    Serial.printf("Checkin %d: %s\n", code, resp.c_str());

    // Apply alarm update if the user changed it on the web UI
    int idx = resp.indexOf("\"alarm\":\"");
    if (idx != -1) {
      idx += 9;
      int end = resp.indexOf("\"", idx);
      if (end != -1) {
        String newAlarm = resp.substring(idx, end);
        if (parseAlarm(newAlarm)) {
          firedToday = false;   // don't skip the new time if it's already passed today
          Serial.printf("Alarm updated by server: %s\n", newAlarm.c_str());
        }
      }
    }

    // Execute manual trigger requested from the web UI
    if (resp.indexOf("\"trigger\":true") != -1) {
      Serial.println("Manual trigger from server");
      pulseOutput();
    }
  } else {
    Serial.printf("Checkin failed: %d\n", code);
  }

  http.end();
}


// ── Setup / loop ──────────────────────────────────────────────────────────────

void setup() {
  Serial.begin(115200);

  pinMode(OUTPUT_PIN, OUTPUT);
  digitalWrite(OUTPUT_PIN, LOW);

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.printf("\nConnected. IP: %s\n", WiFi.localIP().toString().c_str());

  setupTime();
}

void loop() {
  unsigned long now = millis();
  if (now - lastCheckinMs >= CHECKIN_INTERVAL_MS) {
    lastCheckinMs = now;
    doCheckin();
  }

  checkLocalAlarm();
  delay(200);
}
