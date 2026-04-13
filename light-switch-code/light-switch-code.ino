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

const char* url = "https://sasha-light-switch-trigger.onrender.com/check";

// Pick your output pin
const int pin = 10;   // safer than GPIO 10 on most ESP32 boards

enum Mode {
  MODE_DAILY_7AM,
  MODE_EVERY_10S
};

// Change this to select behavior
Mode mode = MODE_EVERY_10S;

// New York time with automatic DST handling
const char* tzInfo = "EST5EDT,M3.2.0/2,M11.1.0/2";

bool firedToday = false;
int lastTenSecondSlot = -1;

void pulseOutput() {
  Serial.println("Trigger HIGH");
  digitalWrite(pin, HIGH);
  delay(1000);
  digitalWrite(pin, LOW);
}

bool getLocalTimeSafe(struct tm* timeinfo) {
  if (!getLocalTime(timeinfo)) {
    Serial.println("Failed to get local time");
    return false;
  }
  return true;
}

void setupTime() {
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  setenv("TZ", tzInfo, 1);
  tzset();

  Serial.print("Waiting for time");
  struct tm timeinfo;
  while (!getLocalTime(&timeinfo)) {
    Serial.print(".");
    delay(500);
  }
  Serial.println();
  Serial.println("Time synced");
}

void checkScheduledTrigger() {
  struct tm timeinfo;
  if (!getLocalTimeSafe(&timeinfo)) return;

  Serial.printf(
    "Local time: %04d-%02d-%02d %02d:%02d:%02d\n",
    timeinfo.tm_year + 1900,
    timeinfo.tm_mon + 1,
    timeinfo.tm_mday,
    timeinfo.tm_hour,
    timeinfo.tm_min,
    timeinfo.tm_sec
  );

  if (mode == MODE_DAILY_7AM) {
    // Fire once per day at 7:00:00 local time
    if (timeinfo.tm_hour == 7 && timeinfo.tm_min == 0 && timeinfo.tm_sec == 0) {
      if (!firedToday) {
        pulseOutput();
        firedToday = true;
      }
    }

    // Reset after 7:00 so it can fire next day
    if (timeinfo.tm_hour != 7) {
      firedToday = false;
    }
  }

  if (mode == MODE_EVERY_10S) {
    // Fire once each 10-second slot: 0,10,20,30,40,50
    int currentSlot = timeinfo.tm_sec / 10;

    if (currentSlot != lastTenSecondSlot) {
      lastTenSecondSlot = currentSlot;
      pulseOutput();
    }
  }
}

void checkHttpTrigger() {
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  http.begin(url);

  int httpCode = http.GET();

  if (httpCode > 0) {
    String payload = http.getString();
    Serial.println("HTTP response:");
    Serial.println(payload);

    if (payload.indexOf("true") != -1) {
      pulseOutput();
    }
  } else {
    Serial.print("HTTP error: ");
    Serial.println(httpCode);
  }

  http.end();
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
  checkScheduledTrigger();
  checkHttpTrigger();

  delay(1000);
}
