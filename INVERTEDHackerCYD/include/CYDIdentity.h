#pragma once
// CYDIdentity.h — Self-identification endpoint for CYD (Cheap Yellow Display) projects.
//
// Usage in your project:
//   #define DEVICE_NAME       "MyProject"
//   #define FIRMWARE_VERSION  "1.0.0"
//   #include "CYDIdentity.h"
//
//   In setup():  identityBegin();
//   In loop():   identityHandle();
//
// Optional: update these globals any time to reflect live state:
//   identity_last_fetch  = unix timestamp of last successful data fetch (0 = never)
//   identity_error_flags = bitmask of app-defined error conditions (0 = none)
//
// Responds to:  GET http://<device-ip>/identify
// Returns JSON: { "name", "mac", "version", "uptime_s", "rssi", "last_fetch", "errors" }

#include <WiFi.h>
#include <WebServer.h>

#ifndef DEVICE_NAME
  #define DEVICE_NAME "UnknownCYD"
#endif
#ifndef FIRMWARE_VERSION
  #define FIRMWARE_VERSION "0.0.0"
#endif

// Writable by the host application to expose live state
unsigned long identity_last_fetch  = 0;   // unix timestamp (or millis-based seconds)
uint32_t      identity_error_flags = 0;

static WebServer _identityServer(80);

static void _handleIdentify() {
  char mac[18];
  uint8_t m[6];
  esp_read_mac(m, ESP_MAC_WIFI_STA);
  snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x",
           m[0], m[1], m[2], m[3], m[4], m[5]);

  unsigned long uptime_s = millis() / 1000UL;
  int rssi = (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : 0;

  char json[256];
  snprintf(json, sizeof(json),
    "{"
      "\"name\":\"%s\","
      "\"mac\":\"%s\","
      "\"version\":\"%s\","
      "\"uptime_s\":%lu,"
      "\"rssi\":%d,"
      "\"last_fetch\":%lu,"
      "\"errors\":%u"
    "}",
    DEVICE_NAME,
    mac,
    FIRMWARE_VERSION,
    uptime_s,
    rssi,
    identity_last_fetch,
    identity_error_flags
  );

  _identityServer.sendHeader("Access-Control-Allow-Origin", "*");
  _identityServer.send(200, "application/json", json);
}

static void _handleNotFound() {
  _identityServer.send(404, "text/plain", "Not found");
}

static void identityBegin() {
  _identityServer.on("/identify", HTTP_GET, _handleIdentify);
  _identityServer.onNotFound(_handleNotFound);
  _identityServer.begin();
  Serial.printf("[Identity] Serving GET /identify on port 80 as \"%s\" v%s\n",
                DEVICE_NAME, FIRMWARE_VERSION);
}

static void identityHandle() {
  _identityServer.handleClient();
}
