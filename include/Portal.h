#pragma once

#include <Arduino_GFX_Library.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>

extern Arduino_GFX *gfx;

// ---------------------------------------------------------------------------
// Persisted settings
// ---------------------------------------------------------------------------
static char    hc_wifi_ssid[64]  = "";
static char    hc_wifi_pass[64]  = "";
static char    hc_font_color[16] = "orange";  // orange|green|blue|cyan|white|multi
static uint8_t hc_font_size      = 1;          // 1=small 2=medium 3=large
static uint8_t hc_last_mode      = 0;          // 0=FEED 4=LIVE (only these two persist)
static bool    hc_led_off        = false;       // true = back RGB LED disabled
static uint8_t hc_brightness     = 200;         // backlight 10–255
static bool    hc_has_settings   = false;

// ---------------------------------------------------------------------------
// Portal state
// ---------------------------------------------------------------------------
static WebServer *portalServer = nullptr;
static DNSServer *portalDNS    = nullptr;
static bool       portalDone   = false;

// ---------------------------------------------------------------------------
// NVS load / save
// ---------------------------------------------------------------------------
static void hcLoadSettings() {
  Preferences prefs;
  prefs.begin("hackercyd", true);
  String ssid   = prefs.getString("ssid",   "");
  String pass   = prefs.getString("pass",   "");
  String fcolor = prefs.getString("fcolor", "orange");
  hc_font_size  = (uint8_t)prefs.getUChar("fsize",  1);
  hc_last_mode  = (uint8_t)prefs.getUChar("mode",   0);
  hc_led_off    =          prefs.getBool  ("ledoff", false);
  hc_brightness = (uint8_t)prefs.getUChar("bright", 200);
  if (hc_brightness < 10) hc_brightness = 10;
  prefs.end();
  ssid.toCharArray(hc_wifi_ssid,    sizeof(hc_wifi_ssid));
  pass.toCharArray(hc_wifi_pass,    sizeof(hc_wifi_pass));
  fcolor.toCharArray(hc_font_color, sizeof(hc_font_color));
  if (hc_font_size < 1 || hc_font_size > 3) hc_font_size = 1;
  if (hc_last_mode != 4) hc_last_mode = 0;  // only FEED(0) or LIVE(4) are valid
  hc_has_settings = (ssid.length() > 0);
}

static void hcSaveSettings(const char* ssid, const char* pass,
                           const char* fcolor, uint8_t fsize, bool led_off, uint8_t brightness) {
  Preferences prefs;
  prefs.begin("hackercyd", false);
  prefs.putString("ssid",   ssid);
  prefs.putString("pass",   pass);
  prefs.putString("fcolor", fcolor);
  prefs.putUChar("fsize",   fsize);
  prefs.putBool("ledoff",   led_off);
  prefs.putUChar("bright",  brightness);
  prefs.end();
  strncpy(hc_wifi_ssid,  ssid,   sizeof(hc_wifi_ssid)    - 1);
  strncpy(hc_wifi_pass,  pass,   sizeof(hc_wifi_pass)    - 1);
  strncpy(hc_font_color, fcolor, sizeof(hc_font_color)   - 1);
  hc_font_size    = fsize;
  hc_led_off      = led_off;
  hc_brightness   = brightness;
  hc_has_settings = true;
}

// Persist just the active mode (called on mode transitions, not via portal).
// Only MODE_FEED (0) and MODE_LIVE (4) are meaningful to restore.
static void hcSaveMode(uint8_t mode) {
  hc_last_mode = mode;
  Preferences prefs;
  prefs.begin("hackercyd", false);
  prefs.putUChar("mode", mode);
  prefs.end();
}

// ---------------------------------------------------------------------------
// On-screen setup instructions
// ---------------------------------------------------------------------------
static void hcShowPortalScreen() {
  gfx->fillScreen(RGB565_BLACK);

  gfx->setTextColor(0xFB20);  // HN orange
  gfx->setTextSize(2);
  gfx->setCursor(20, 8);
  gfx->print("HackerCYD Setup");

  gfx->setTextColor(RGB565_WHITE);
  gfx->setTextSize(1);
  gfx->setCursor(50, 32);
  gfx->print("Hacker News for CYD");

  gfx->setTextColor(0xFFE0);
  gfx->setCursor(4, 54);
  gfx->print("1. Connect to WiFi:");
  gfx->setTextColor(0xFB20);
  gfx->setTextSize(2);
  gfx->setCursor(14, 66);
  gfx->print("HackerCYD_Setup");

  gfx->setTextColor(0xFFE0);
  gfx->setTextSize(1);
  gfx->setCursor(4, 92);
  gfx->print("2. Open browser:");
  gfx->setTextColor(0xFB20);
  gfx->setTextSize(2);
  gfx->setCursor(50, 104);
  gfx->print("192.168.4.1");

  gfx->setTextColor(0xFFE0);
  gfx->setTextSize(1);
  gfx->setCursor(4, 130);
  gfx->print("3. Enter WiFi and tap Save.");

  if (hc_has_settings) {
    gfx->setTextColor(0x07E0);
    gfx->setCursor(4, 154);
    gfx->print("Existing settings found.");
    gfx->setCursor(4, 166);
    gfx->print("Tap 'No Changes' to keep.");
  }
}

// ---------------------------------------------------------------------------
// Web handlers
// ---------------------------------------------------------------------------
static void hcHandleRoot() {
  String html =
    "<!DOCTYPE html><html><head>"
    "<meta charset='UTF-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>HackerCYD Setup</title>"
    "<style>"
    "body{background:#0d0d0d;color:#ff6600;font-family:Arial,sans-serif;"
         "text-align:center;padding:20px;max-width:480px;margin:auto;}"
    "h1{color:#ff6600;font-size:1.6em;margin-bottom:4px;}"
    "p{color:#884422;font-size:0.9em;}"
    "label{display:block;text-align:left;margin:14px 0 4px;color:#ffaa66;"
          "font-weight:bold;}"
    "input[type=text],input[type=password]{width:100%;box-sizing:border-box;"
          "background:#1a0800;color:#ff8833;border:2px solid #662200;"
          "border-radius:6px;padding:10px;font-size:1em;}"
    ".btn{display:block;width:100%;padding:14px;margin:10px 0;font-size:1.05em;"
         "border-radius:8px;border:none;cursor:pointer;font-weight:bold;}"
    ".btn-save{background:#331100;color:#ff6600;border:2px solid #882200;}"
    ".btn-save:hover{background:#552200;}"
    ".btn-skip{background:#111;color:#555;border:2px solid #333;}"
    ".btn-skip:hover{background:#222;color:#888;}"
    ".note{color:#443322;font-size:0.82em;margin-top:16px;}"
    "hr{border:1px solid #331100;margin:20px 0;}"
    ".rng{display:flex;align-items:center;gap:8px;margin:6px 0 14px;}"
    ".rng input[type=range]{flex:1;accent-color:#ff6600;}"
    ".rng output{min-width:28px;text-align:right;color:#ff8833;}"
    ".rg{text-align:left;margin:6px 0 14px;display:flex;flex-wrap:wrap;gap:4px 18px;}"
    ".rl{color:#ff8833;cursor:pointer;font-weight:normal;margin:0;}"
    ".rl input{width:auto;border:none;padding:0;background:none;margin-right:4px;}"
    "</style></head><body>"
    "<h1>&#129490; HackerCYD</h1>"
    "<p>Hacker News top stories on your CYD.</p>"
    "<form method='post' action='/save'>"
    "<label>WiFi Network (SSID):</label>"
    "<input type='text' name='ssid' value='";
  html += String(hc_wifi_ssid);
  html +=
    "' placeholder='Your 2.4 GHz WiFi name' maxlength='63' required>"
    "<label>WiFi Password:</label>"
    "<input type='password' name='pass' value='";
  html += String(hc_wifi_pass);
  html +=
    "' placeholder='Leave blank if open network' maxlength='63'>"
    "<br>";

  // --- Font color theme ---
  html += "<label>Font Color Theme:</label><div class='rg'>";
  const char* colorVals[]   = {"orange","green","blue","cyan","white","multi"};
  const char* colorLabels[] = {
    "&#127818; Orange", "&#129376; Green", "&#128153; Blue",
    "&#128306; Cyan",   "&#11036; White",  "&#127752; Multicolor (each line)"
  };
  for (int i = 0; i < 6; i++) {
    html += "<label class='rl'><input type='radio' name='fcolor' value='";
    html += colorVals[i]; html += "'";
    if (strcmp(hc_font_color, colorVals[i]) == 0) html += " checked";
    html += "> "; html += colorLabels[i]; html += "</label>";
  }
  html += "</div>";

  // --- Font size ---
  html += "<label>Font Size:</label><div class='rg'>";
  const char* sizeVals[]   = {"1","2","3"};
  const char* sizeLabels[] = {"Small","Medium","Large"};
  for (int i = 0; i < 3; i++) {
    html += "<label class='rl'><input type='radio' name='fsize' value='";
    html += sizeVals[i]; html += "'";
    if (hc_font_size == (uint8_t)(i + 1)) html += " checked";
    html += "> "; html += sizeLabels[i]; html += "</label>";
  }
  html += "</div>";

  // --- Back LED ---
  html += "<label>Back RGB LED:</label><div class='rg'>"
          "<label class='rl'><input type='checkbox' name='ledoff' value='1'";
  if (hc_led_off) html += " checked";
  html += "> &#128161; Disable back LED (saves power)</label></div>";

  // --- Brightness ---
  html += "<label>Brightness:</label><div class='rng'>"
          "<input type='range' name='bright' min='10' max='255' value='";
  html += String(hc_brightness);
  html += "' oninput='this.nextElementSibling.value=this.value'>"
          "<output>";
  html += String(hc_brightness);
  html += "</output></div><br>";

  html +=
    "<button class='btn btn-save' type='submit'>&#128190; Save &amp; Connect</button>"
    "</form>";

  if (hc_has_settings) {
    html +=
      "<hr>"
      "<form method='post' action='/nochange'>"
      "<button class='btn btn-skip' type='submit'>"
      "&#10006; No Changes &mdash; Use Current Settings"
      "</button></form>";
  }

  html +=
    "<p class='note'>&#9888; ESP32 supports 2.4 GHz WiFi only.</p>"
    "</body></html>";

  portalServer->send(200, "text/html", html);
}

static void hcHandleSave() {
  String ssid = portalServer->hasArg("ssid") ? portalServer->arg("ssid") : "";
  String pass = portalServer->hasArg("pass") ? portalServer->arg("pass") : "";

  if (ssid.length() == 0) {
    portalServer->send(400, "text/html",
      "<html><body style='background:#0d0d0d;color:#ff5555;font-family:Arial;"
      "text-align:center;padding:40px'>"
      "<h2>&#10060; SSID cannot be empty!</h2>"
      "<a href='/' style='color:#ff6600'>&#8592; Go Back</a></body></html>");
    return;
  }

  hcSaveSettings(ssid.c_str(), pass.c_str(),
    (portalServer->hasArg("fcolor") ? portalServer->arg("fcolor").c_str() : "orange"),
    (portalServer->hasArg("fsize")  ? (uint8_t)constrain(portalServer->arg("fsize").toInt(),1,3) : 1),
    portalServer->hasArg("ledoff"),
    (portalServer->hasArg("bright") ? (uint8_t)constrain(portalServer->arg("bright").toInt(),10,255) : 200));

  portalServer->send(200, "text/html",
    "<html><head><meta charset='UTF-8'>"
    "<style>body{background:#0d0d0d;color:#ff6600;font-family:Arial;"
    "text-align:center;padding:40px;}"
    "h2{color:#ff8833;}p{color:#884422;}</style></head><body>"
    "<h2>&#9989; Settings Saved!</h2>"
    "<p>Connecting to <b>" + ssid + "</b>...</p>"
    "<p>You can close this page and disconnect from <b>HackerCYD_Setup</b>.</p>"
    "</body></html>");

  delay(1500);
  portalDone = true;
}

static void hcHandleNoChange() {
  portalServer->send(200, "text/html",
    "<html><head><meta charset='UTF-8'>"
    "<style>body{background:#0d0d0d;color:#ff6600;font-family:Arial;"
    "text-align:center;padding:40px;}"
    "h2{color:#ff8833;}p{color:#884422;}</style></head><body>"
    "<h2>&#128077; No Changes</h2>"
    "<p>Using saved settings. Device connecting now.</p>"
    "<p>Disconnect from <b>HackerCYD_Setup</b>.</p>"
    "</body></html>");
  delay(1500);
  portalDone = true;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
static void hcInitPortal() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP("HackerCYD_Setup", "");
  delay(500);

  portalDNS    = new DNSServer();
  portalServer = new WebServer(80);

  portalDNS->start(53, "*", WiFi.softAPIP());
  portalServer->on("/",         hcHandleRoot);
  portalServer->on("/save",     HTTP_POST, hcHandleSave);
  portalServer->on("/nochange", HTTP_POST, hcHandleNoChange);
  portalServer->onNotFound(hcHandleRoot);
  portalServer->begin();

  portalDone = false;
  hcShowPortalScreen();

  Serial.printf("[Portal] AP up — connect to HackerCYD_Setup, open %s\n",
                WiFi.softAPIP().toString().c_str());
}

static void hcRunPortal()   { portalDNS->processNextRequest(); portalServer->handleClient(); }

static void hcClosePortal() {
  portalServer->stop();
  portalDNS->stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(300);
  delete portalServer; portalServer = nullptr;
  delete portalDNS;    portalDNS    = nullptr;
}
