#pragma once

#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <WiFi.h>

// Ensure WiFi is up and has a valid IP before any fetch.
// Reconnects if disconnected or DHCP hasn't completed.
static bool hn_ensure_wifi() {
  if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0))
    return true;
  WiFi.disconnect();
  delay(200);
  WiFi.reconnect();
  uint32_t t = millis();
  while ((WiFi.status() != WL_CONNECTED || WiFi.localIP() == IPAddress(0, 0, 0, 0))
         && millis() - t < 12000)
    delay(300);
  return WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0);
}

// ---------------------------------------------------------------------------
// Story data
// ---------------------------------------------------------------------------
#define MAX_STORIES 15

struct Story {
  char    title[120];
  char    author[32];
  char    domain[48];
  char    objectId[12];  // HN item ID — used to build QR code URL
  int16_t points;
  int16_t comments;
  int32_t age_hours;   // hours since posted (computed at fetch time)
};

static Story hnStories[MAX_STORIES];
static int   hnCount        = 0;
static int   hnLastHttpCode = 0;
static char  hnLastErr[20]  = "";

// ---------------------------------------------------------------------------
// Comment data — fetched on demand when user enters CMTS mode
// ---------------------------------------------------------------------------
#define MAX_COMMENTS 8

struct Comment {
  char    author[32];
  char    text[1024];  // plain text after HTML stripping
  int32_t age_hours;
};

static Comment hnComments[MAX_COMMENTS];
static int     hnCommentCount = 0;
static char    hnCmtStoryId[12] = "";  // story whose comments are loaded

// ---------------------------------------------------------------------------
// Extract domain from a URL: "https://www.example.com/path" → "example.com"
// Null/empty URL → "self post"
// ---------------------------------------------------------------------------
static void extractDomain(const char* url, char* out, size_t outLen) {
  if (!url || strlen(url) == 0) {
    strncpy(out, "self post", outLen - 1);
    out[outLen - 1] = '\0';
    return;
  }
  const char* p = strstr(url, "://");
  if (!p) { strncpy(out, url, outLen - 1); out[outLen - 1] = '\0'; return; }
  p += 3;
  if (strncmp(p, "www.", 4) == 0) p += 4;
  const char* slash = strchr(p, '/');
  size_t len = slash ? (size_t)(slash - p) : strlen(p);
  if (len >= outLen) len = outLen - 1;
  strncpy(out, p, len);
  out[len] = '\0';
}

// ---------------------------------------------------------------------------
// Strip HTML tags and decode common entities from HN comment_text.
// HN stores comments as HTML: <p>text</p><p>more</p>
// We turn each </p> into two newlines so paragraphs are separated.
// ---------------------------------------------------------------------------
static void stripHtml(const char* html, char* out, size_t outLen) {
  size_t j = 0;
  bool inTag = false;
  for (size_t i = 0; html[i] && j < outLen - 2; i++) {
    if (html[i] == '<') {
      // Closing </p> → paragraph break
      if (strncasecmp(&html[i], "</p>", 4) == 0 && j > 0 && out[j-1] != '\n') {
        out[j++] = '\n';
      }
      inTag = true;
      continue;
    }
    if (html[i] == '>') { inTag = false; continue; }
    if (inTag) continue;
    // Decode common entities
    if (html[i] == '&') {
      if      (strncmp(&html[i], "&amp;",  5) == 0) { out[j++] = '&';  i += 4; continue; }
      else if (strncmp(&html[i], "&lt;",   4) == 0) { out[j++] = '<';  i += 3; continue; }
      else if (strncmp(&html[i], "&gt;",   4) == 0) { out[j++] = '>';  i += 3; continue; }
      else if (strncmp(&html[i], "&quot;", 6) == 0) { out[j++] = '"';  i += 5; continue; }
      else if (strncmp(&html[i], "&#x27;", 6) == 0) { out[j++] = '\''; i += 5; continue; }
      else if (strncmp(&html[i], "&apos;", 6) == 0) { out[j++] = '\''; i += 5; continue; }
      else if (strncmp(&html[i], "&#x2F;", 6) == 0) { out[j++] = '/';  i += 5; continue; }
    }
    out[j++] = html[i];
  }
  while (j > 0 && (out[j-1] == '\n' || out[j-1] == ' ')) j--;
  out[j] = '\0';
}

// ---------------------------------------------------------------------------
// Fetch top comments for a story by HN objectId.
// Endpoint: /api/v1/search?tags=comment,story_ID&hitsPerPage=8
// Returns number of comments loaded, -1 on network error.
// ---------------------------------------------------------------------------
static int hnFetchComments(const char* objectId) {
  hnCommentCount = 0;
  strncpy(hnCmtStoryId, objectId, sizeof(hnCmtStoryId) - 1);
  if (!hn_ensure_wifi()) { hnLastHttpCode = -1; return -1; }

  char path[80];
  snprintf(path, sizeof(path),
           "/api/v1/search_by_date?tags=comment,story_%s&hitsPerPage=8", objectId);

  WiFiClientSecure client;
  client.setInsecure();

  if (!client.connect("hn.algolia.com", 443, 15000)) {
    hnLastHttpCode = -1;
    return -1;
  }
  client.setTimeout(15000);

  client.print("GET ");
  client.print(path);
  client.print(" HTTP/1.0\r\n"
               "Host: hn.algolia.com\r\n"
               "User-Agent: HackerCYD/1.0 ESP32\r\n"
               "Connection: close\r\n"
               "\r\n");

  String statusLine = client.readStringUntil('\n');
  statusLine.trim();
  hnLastHttpCode = 0;
  if (statusLine.startsWith("HTTP/")) {
    int sp = statusLine.indexOf(' ');
    if (sp > 0) hnLastHttpCode = statusLine.substring(sp + 1, sp + 4).toInt();
  }
  if (hnLastHttpCode != 200) { client.stop(); return -1; }

  while (client.connected() || client.available()) {
    String line = client.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) break;
  }

  DynamicJsonDocument filter(256);
  filter["hits"][0]["comment_text"] = true;
  filter["hits"][0]["author"]       = true;
  filter["hits"][0]["created_at_i"] = true;

  DynamicJsonDocument doc(24576);
  DeserializationError err = deserializeJson(doc, client,
                               DeserializationOption::Filter(filter),
                               DeserializationOption::NestingLimit(20));
  client.stop();

  if (err) {
    strncpy(hnLastErr, err.c_str(), sizeof(hnLastErr) - 1);
    return -2;
  }
  hnLastErr[0] = '\0';

  uint32_t now = (uint32_t)time(nullptr);
  for (JsonObject hit : doc["hits"].as<JsonArray>()) {
    if (hnCommentCount >= MAX_COMMENTS) break;

    const char* html   = hit["comment_text"] | "";
    const char* author = hit["author"]       | "unknown";
    if (strlen(html) == 0) continue;  // skip deleted/empty comments

    Comment &c = hnComments[hnCommentCount];
    memset(&c, 0, sizeof(c));
    strncpy(c.author, author, sizeof(c.author) - 1);
    stripHtml(html, c.text, sizeof(c.text));

    uint32_t created = hit["created_at_i"] | 0u;
    c.age_hours = (now > created && created > 0)
                  ? (int32_t)((now - created) / 3600) : 0;
    hnCommentCount++;
  }

  Serial.printf("[HN] fetched %d comments for story %s\n", hnCommentCount, objectId);
  return hnCommentCount;
}

// ---------------------------------------------------------------------------
// Uses raw WiFiClientSecure + HTTP/1.0 (same technique as SportsCYD ESPN fetch)
// so the server skips chunked transfer encoding and streams until close.
//
// Endpoint: https://hn.algolia.com/api/v1/search?tags=front_page&hitsPerPage=15
// Response: { "hits": [ { title, url, points, author, num_comments,
//                          created_at_i (unix timestamp) }, ... ] }
// ---------------------------------------------------------------------------
static int hnFetch() {
  const char* host = "hn.algolia.com";
  const char* path = "/api/v1/search?tags=front_page&hitsPerPage=15";
  if (!hn_ensure_wifi()) { hnLastHttpCode = -1; return -1; }

  WiFiClientSecure client;
  client.setInsecure();

  if (!client.connect(host, 443, 15000)) {
    Serial.println("[HN] connect failed");
    hnLastHttpCode = -1;
    return -1;
  }

  // Extend read timeout so ArduinoJson's timedRead() doesn't give up
  // between TCP packets
  client.setTimeout(15000);

  client.print("GET ");
  client.print(path);
  client.print(" HTTP/1.0\r\n"
               "Host: hn.algolia.com\r\n"
               "User-Agent: HackerCYD/1.0 ESP32\r\n"
               "Connection: close\r\n"
               "\r\n");

  // Read status line
  String statusLine = client.readStringUntil('\n');
  statusLine.trim();
  Serial.printf("[HN] %s\n", statusLine.c_str());

  hnLastHttpCode = 0;
  if (statusLine.startsWith("HTTP/")) {
    int sp = statusLine.indexOf(' ');
    if (sp > 0) hnLastHttpCode = statusLine.substring(sp + 1, sp + 4).toInt();
  }
  if (hnLastHttpCode != 200) {
    client.stop();
    return -1;
  }

  // Discard all response headers
  while (client.connected() || client.available()) {
    String line = client.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) break;
  }

  // Filter: only pull the 6 fields we display
  DynamicJsonDocument filter(512);
  filter["hits"][0]["title"]        = true;
  filter["hits"][0]["url"]          = true;
  filter["hits"][0]["points"]       = true;
  filter["hits"][0]["author"]       = true;
  filter["hits"][0]["num_comments"] = true;
  filter["hits"][0]["created_at_i"] = true;  // unix timestamp int
  filter["hits"][0]["objectID"]     = true;  // HN item ID for QR code URL

  DynamicJsonDocument doc(8192);
  DeserializationError err = deserializeJson(doc, client,
                               DeserializationOption::Filter(filter),
                               DeserializationOption::NestingLimit(20));
  client.stop();

  if (err) {
    strncpy(hnLastErr, err.c_str(), sizeof(hnLastErr) - 1);
    Serial.printf("[HN] JSON error: %s\n", err.c_str());
    return -2;
  }
  hnLastErr[0] = '\0';

  hnCount = 0;
  uint32_t now = (uint32_t)time(nullptr);

  for (JsonObject hit : doc["hits"].as<JsonArray>()) {
    if (hnCount >= MAX_STORIES) break;

    Story &s = hnStories[hnCount];
    memset(&s, 0, sizeof(s));

    const char* title    = hit["title"]    | "";
    const char* url      = hit["url"]      | "";
    const char* author   = hit["author"]   | "unknown";
    const char* objectId = hit["objectID"] | "";

    strncpy(s.title,    title,    sizeof(s.title)    - 1);
    strncpy(s.author,   author,   sizeof(s.author)   - 1);
    strncpy(s.objectId, objectId, sizeof(s.objectId) - 1);
    extractDomain(url, s.domain, sizeof(s.domain));

    s.points   = (int16_t)(hit["points"]       | 0);
    s.comments = (int16_t)(hit["num_comments"] | 0);

    uint32_t created = hit["created_at_i"] | 0u;
    s.age_hours = (now > created && created > 0)
                  ? (int32_t)((now - created) / 3600)
                  : 0;

    hnCount++;
  }

  Serial.printf("[HN] fetched %d stories\n", hnCount);
  return hnCount;
}

// ---------------------------------------------------------------------------
// Live comment feed — most recent comments posted site-wide across all stories
// Endpoint: /api/v1/search_by_date?tags=comment&hitsPerPage=8
// ---------------------------------------------------------------------------
#define MAX_LIVE_COMMENTS 25

struct LiveComment {
  char    author[32];
  char    story_title[80];
  char    story_id[12];   // HN story objectID — used to lock HOLD onto one thread
  char    text[512];
  int32_t age_minutes;  // minutes since posted (0 = just now)
};

static LiveComment hnLiveComments[MAX_LIVE_COMMENTS];
static int         hnLiveCount = 0;

static int hnFetchLive(int hitsPerPage = 8, const char* storyId = nullptr) {
  const char* host = "hn.algolia.com";
  if (!hn_ensure_wifi()) return -1;
  char path[96];
  if (storyId && strlen(storyId) > 0) {
    snprintf(path, sizeof(path),
             "/api/v1/search_by_date?tags=comment,story_%s&hitsPerPage=%d", storyId, hitsPerPage);
  } else {
    snprintf(path, sizeof(path),
             "/api/v1/search_by_date?tags=comment&hitsPerPage=%d", hitsPerPage);
  }

  WiFiClientSecure client;
  client.setInsecure();
  if (!client.connect(host, 443, 15000)) return -1;
  client.setTimeout(15000);

  client.print("GET ");
  client.print(path);
  client.print(" HTTP/1.0\r\n"
               "Host: hn.algolia.com\r\n"
               "User-Agent: HackerCYD/1.0 ESP32\r\n"
               "Connection: close\r\n"
               "\r\n");

  String statusLine = client.readStringUntil('\n');
  statusLine.trim();
  int code = 0;
  if (statusLine.startsWith("HTTP/")) {
    int sp = statusLine.indexOf(' ');
    if (sp > 0) code = statusLine.substring(sp + 1, sp + 4).toInt();
  }
  if (code != 200) { client.stop(); return -1; }

  while (client.connected() || client.available()) {
    String line = client.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) break;
  }

  DynamicJsonDocument filter(256);
  filter["hits"][0]["comment_text"]  = true;
  filter["hits"][0]["author"]        = true;
  filter["hits"][0]["story_title"]   = true;
  filter["hits"][0]["story_id"]      = true;
  filter["hits"][0]["created_at_i"]  = true;

  DynamicJsonDocument doc(32768);
  DeserializationError err = deserializeJson(doc, client,
                               DeserializationOption::Filter(filter),
                               DeserializationOption::NestingLimit(20));
  client.stop();
  if (err) return -2;

  hnLiveCount = 0;
  uint32_t now = (uint32_t)time(nullptr);
  for (JsonObject hit : doc["hits"].as<JsonArray>()) {
    if (hnLiveCount >= MAX_LIVE_COMMENTS) break;
    const char* html    = hit["comment_text"] | "";
    const char* author  = hit["author"]       | "unknown";
    const char* stitle  = hit["story_title"]  | "";
    int32_t     sid_int = hit["story_id"]     | 0;   // story_id is an integer in Algolia
    if (strlen(html) == 0) continue;

    LiveComment &lc = hnLiveComments[hnLiveCount];
    memset(&lc, 0, sizeof(lc));
    strncpy(lc.author,      author, sizeof(lc.author)      - 1);
    strncpy(lc.story_title, stitle, sizeof(lc.story_title) - 1);
    if (sid_int > 0) snprintf(lc.story_id, sizeof(lc.story_id), "%d", sid_int);
    stripHtml(html, lc.text, sizeof(lc.text));

    uint32_t created = hit["created_at_i"] | 0u;
    lc.age_minutes = (now > created && created > 0)
                     ? (int32_t)((now - created) / 60) : 0;
    hnLiveCount++;
  }
  Serial.printf("[HN] fetched %d live comments\n", hnLiveCount);
  return hnLiveCount;
}
