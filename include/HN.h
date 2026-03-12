#pragma once

#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <WiFi.h>

// Official HN Firebase API — maintained by Y Combinator, no rate limits, no key needed.
// Replaced hn.algolia.com which was archived Feb 2026 and experiencing outages.
#define HN_FIREBASE_HOST "hacker-news.firebaseio.com"

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
// Open one HTTPS GET to HN_FIREBASE_HOST, skip response headers.
// Client must be freshly constructed. Returns HTTP status (200 on success).
// ---------------------------------------------------------------------------
static int hn_firebase_get(WiFiClientSecure& client, const char* path) {
  client.setInsecure();
  if (!client.connect(HN_FIREBASE_HOST, 443, 15000)) return 0;
  client.setTimeout(15000);
  client.print("GET ");
  client.print(path);
  client.print(" HTTP/1.0\r\n"
               "Host: " HN_FIREBASE_HOST "\r\n"
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
  if (code != 200) { client.stop(); return code ? code : -1; }
  // Discard response headers
  while (client.connected() || client.available()) {
    String line = client.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) break;
  }
  return 200;
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
// Fetch top comments for a story from Firebase.
// Step 1: GET /v0/item/{storyId}.json  → { "kids": [commentId, ...] }
// Step 2: For each kid, GET /v0/item/{kidId}.json  → comment fields
// ---------------------------------------------------------------------------
static int hnFetchComments(const char* objectId) {
  hnCommentCount = 0;
  strncpy(hnCmtStoryId, objectId, sizeof(hnCmtStoryId) - 1);
  if (!hn_ensure_wifi()) { hnLastHttpCode = -1; return -1; }

  // Step 1: get story item to retrieve its kids list
  char path[40];
  snprintf(path, sizeof(path), "/v0/item/%s.json", objectId);

  WiFiClientSecure sc;
  int code = hn_firebase_get(sc, path);
  hnLastHttpCode = code;
  if (code != 200) return -1;

  DynamicJsonDocument sfilter(64);
  sfilter["kids"] = true;

  // kids array can be large; 6 KB handles ~750 comment IDs comfortably
  DynamicJsonDocument sdoc(6144);
  DeserializationError err = deserializeJson(sdoc, sc,
                               DeserializationOption::Filter(sfilter),
                               DeserializationOption::NestingLimit(5));
  sc.stop();
  if (err) {
    strncpy(hnLastErr, err.c_str(), sizeof(hnLastErr) - 1);
    return -2;
  }

  JsonArray kids = sdoc["kids"].as<JsonArray>();
  if (kids.isNull()) return 0;

  // Step 2: fetch each top-level comment
  uint32_t now   = (uint32_t)time(nullptr);
  int      tried = 0;

  for (JsonVariant kidV : kids) {
    if (hnCommentCount >= MAX_COMMENTS) break;
    if (tried          >= MAX_COMMENTS + 4) break;
    tried++;

    int32_t kid = kidV.as<int32_t>();
    if (kid <= 0) continue;

    char cpath[40];
    snprintf(cpath, sizeof(cpath), "/v0/item/%ld.json", (long)kid);

    WiFiClientSecure cc;
    if (hn_firebase_get(cc, cpath) != 200) { cc.stop(); continue; }

    DynamicJsonDocument cfilter(96);
    cfilter["by"]      = true;
    cfilter["text"]    = true;
    cfilter["time"]    = true;
    cfilter["type"]    = true;
    cfilter["dead"]    = true;
    cfilter["deleted"] = true;

    DynamicJsonDocument cdoc(4096);
    err = deserializeJson(cdoc, cc,
                          DeserializationOption::Filter(cfilter),
                          DeserializationOption::NestingLimit(5));
    cc.stop();
    if (err) continue;

    const char* type = cdoc["type"] | "";
    if (strcmp(type, "comment") != 0) continue;
    if (cdoc["dead"]    | false) continue;
    if (cdoc["deleted"] | false) continue;

    const char* html   = cdoc["text"] | "";
    const char* author = cdoc["by"]   | "unknown";
    if (strlen(html) == 0) continue;

    Comment& c = hnComments[hnCommentCount];
    memset(&c, 0, sizeof(c));
    strncpy(c.author, author, sizeof(c.author) - 1);
    stripHtml(html, c.text, sizeof(c.text));

    uint32_t created = cdoc["time"] | 0u;
    c.age_hours = (now > created && created > 0)
                  ? (int32_t)((now - created) / 3600) : 0;
    hnCommentCount++;
  }

  Serial.printf("[HN] fetched %d comments for story %s\n", hnCommentCount, objectId);
  return hnCommentCount;
}

// ---------------------------------------------------------------------------
// Fetch top 15 stories from the official HN Firebase API.
// Step 1: GET /v0/topstories.json  → JSON array of up to 500 item IDs
// Step 2: For each of the first MAX_STORIES IDs, GET /v0/item/{id}.json
//
// The Firebase API uses HTTP/1.0 cleanly (no chunked encoding) and is
// maintained by Y Combinator with no rate limits or API key required.
// ---------------------------------------------------------------------------
static int hnFetch() {
  if (!hn_ensure_wifi()) { hnLastHttpCode = -1; return -1; }

  // Step 1: fetch the ranked ID list
  WiFiClientSecure lc;
  int code = hn_firebase_get(lc, "/v0/topstories.json");
  hnLastHttpCode = code;
  if (code != 200) { return -1; }

  // Parse array of IDs — 500 integers, ~8 KB with ArduinoJson v6 overhead
  DynamicJsonDocument idsDoc(8192);
  DeserializationError err = deserializeJson(idsDoc, lc);
  lc.stop();
  if (err) {
    strncpy(hnLastErr, err.c_str(), sizeof(hnLastErr) - 1);
    Serial.printf("[HN] IDs JSON error: %s\n", err.c_str());
    return -2;
  }
  hnLastErr[0] = '\0';

  JsonArray ids = idsDoc.as<JsonArray>();
  if (ids.isNull() || ids.size() == 0) return 0;

  // Step 2: fetch each item individually
  hnCount = 0;
  uint32_t now   = (uint32_t)time(nullptr);
  int      tried = 0;

  for (JsonVariant v : ids) {
    if (hnCount  >= MAX_STORIES)       break;
    if (tried    >= MAX_STORIES + 5)   break;  // skip a few extras for dead/deleted
    tried++;

    int32_t id = v.as<int32_t>();
    if (id <= 0) continue;

    char path[40];
    snprintf(path, sizeof(path), "/v0/item/%ld.json", (long)id);

    WiFiClientSecure ic;
    int icode = hn_firebase_get(ic, path);
    if (icode != 200) { ic.stop(); continue; }

    DynamicJsonDocument filter(128);
    filter["title"]       = true;
    filter["url"]         = true;
    filter["by"]          = true;
    filter["score"]       = true;
    filter["descendants"] = true;
    filter["time"]        = true;
    filter["type"]        = true;
    filter["dead"]        = true;
    filter["deleted"]     = true;

    DynamicJsonDocument doc(1024);
    err = deserializeJson(doc, ic,
                          DeserializationOption::Filter(filter),
                          DeserializationOption::NestingLimit(5));
    ic.stop();
    if (err) continue;

    const char* type = doc["type"] | "";
    if (strcmp(type, "story") != 0) continue;
    if (doc["dead"]    | false)     continue;
    if (doc["deleted"] | false)     continue;

    const char* title  = doc["title"] | "";
    const char* url    = doc["url"]   | "";
    const char* author = doc["by"]    | "unknown";
    if (strlen(title) == 0) continue;

    Story& s = hnStories[hnCount];
    memset(&s, 0, sizeof(s));
    strncpy(s.title,  title,  sizeof(s.title)  - 1);
    strncpy(s.author, author, sizeof(s.author) - 1);
    snprintf(s.objectId, sizeof(s.objectId), "%ld", (long)id);
    extractDomain(url, s.domain, sizeof(s.domain));

    s.points   = (int16_t)(doc["score"]       | 0);
    s.comments = (int16_t)(doc["descendants"] | 0);

    uint32_t created = doc["time"] | 0u;
    s.age_hours = (now > created && created > 0)
                  ? (int32_t)((now - created) / 3600) : 0;
    hnCount++;
  }

  Serial.printf("[HN] fetched %d stories\n", hnCount);
  return hnCount;
}

// ---------------------------------------------------------------------------
// Live comment feed via Firebase /v0/updates.json
// Returns recently-updated item IDs; we filter for type=comment.
// When storyId is provided, fetches that story's direct-reply kids instead.
// Note: story_title is not populated (Firebase requires extra parent traversal).
// ---------------------------------------------------------------------------
#define MAX_LIVE_COMMENTS 25

struct LiveComment {
  char    author[32];
  char    story_title[80];
  char    story_id[12];
  char    text[512];
  int32_t age_minutes;
};

static LiveComment hnLiveComments[MAX_LIVE_COMMENTS];
static int         hnLiveCount = 0;

static int hnFetchLive(int hitsPerPage = 8, const char* storyId = nullptr) {
  if (!hn_ensure_wifi()) return -1;
  hnLiveCount = 0;
  uint32_t now = (uint32_t)time(nullptr);

  // Helper lambda-style local: fetch one comment item and append to hnLiveComments
  // (implemented inline in each branch below)

  if (storyId && strlen(storyId) > 0) {
    // Fetch direct-reply comments for a specific story
    char path[40];
    snprintf(path, sizeof(path), "/v0/item/%s.json", storyId);

    WiFiClientSecure sc;
    if (hn_firebase_get(sc, path) != 200) { sc.stop(); return -1; }

    DynamicJsonDocument sfilter(64);
    sfilter["kids"] = true;
    DynamicJsonDocument sdoc(6144);
    DeserializationError err = deserializeJson(sdoc, sc,
                                 DeserializationOption::Filter(sfilter),
                                 DeserializationOption::NestingLimit(5));
    sc.stop();
    if (err) return -2;

    JsonArray kids = sdoc["kids"].as<JsonArray>();
    if (kids.isNull()) return 0;

    int tried = 0;
    for (JsonVariant kidV : kids) {
      if (hnLiveCount >= hitsPerPage || hnLiveCount >= MAX_LIVE_COMMENTS) break;
      if (tried >= hitsPerPage + 4) break;
      tried++;

      int32_t kid = kidV.as<int32_t>();
      if (kid <= 0) continue;

      char cpath[40];
      snprintf(cpath, sizeof(cpath), "/v0/item/%ld.json", (long)kid);

      WiFiClientSecure cc;
      if (hn_firebase_get(cc, cpath) != 200) { cc.stop(); continue; }

      DynamicJsonDocument cfilter(96);
      cfilter["by"]      = true;
      cfilter["text"]    = true;
      cfilter["time"]    = true;
      cfilter["type"]    = true;
      cfilter["dead"]    = true;
      cfilter["deleted"] = true;

      DynamicJsonDocument cdoc(2048);
      err = deserializeJson(cdoc, cc,
                            DeserializationOption::Filter(cfilter),
                            DeserializationOption::NestingLimit(5));
      cc.stop();
      if (err) continue;

      const char* type = cdoc["type"] | "";
      if (strcmp(type, "comment") != 0) continue;
      if (cdoc["dead"]    | false) continue;
      if (cdoc["deleted"] | false) continue;
      const char* html   = cdoc["text"] | "";
      const char* author = cdoc["by"]   | "unknown";
      if (strlen(html) == 0) continue;

      LiveComment& lc = hnLiveComments[hnLiveCount];
      memset(&lc, 0, sizeof(lc));
      strncpy(lc.author,   author,  sizeof(lc.author)   - 1);
      strncpy(lc.story_id, storyId, sizeof(lc.story_id) - 1);
      stripHtml(html, lc.text, sizeof(lc.text));
      uint32_t created = cdoc["time"] | 0u;
      lc.age_minutes = (now > created && created > 0)
                       ? (int32_t)((now - created) / 60) : 0;
      hnLiveCount++;
    }

  } else {
    // Walk backwards from the highest item ID ever created.
    // Items are created with sequential IDs, so maxitem - N = the Nth most recent item.
    // HN is comment-heavy, so most recent IDs will be comments on active stories.

    // Step 1: get the current max item ID
    WiFiClientSecure mc;
    if (hn_firebase_get(mc, "/v0/maxitem.json") != 200) { mc.stop(); return -1; }
    DynamicJsonDocument maxDoc(32);
    DeserializationError err = deserializeJson(maxDoc, mc);
    mc.stop();
    if (err) return -1;
    int32_t maxId = maxDoc.as<int32_t>();
    if (maxId <= 0) return -1;
    Serial.printf("[HN] maxitem=%ld\n", (long)maxId);

    // Step 2: walk backwards, collect comments (newest first)
    // Try up to hitsPerPage*4 IDs — on active HN ~60-70% of recent IDs are comments,
    // so 8 comments from 32 tries is a comfortable margin.
    int tried = 0;
    int maxTry = hitsPerPage * 4;

    for (int32_t id = maxId;
         id > 0 && hnLiveCount < hitsPerPage && hnLiveCount < MAX_LIVE_COMMENTS && tried < maxTry;
         id--, tried++) {

      char cpath[40];
      snprintf(cpath, sizeof(cpath), "/v0/item/%ld.json", (long)id);

      WiFiClientSecure cc;
      if (hn_firebase_get(cc, cpath) != 200) { cc.stop(); continue; }

      DynamicJsonDocument cfilter(128);
      cfilter["by"]      = true;
      cfilter["text"]    = true;
      cfilter["time"]    = true;
      cfilter["type"]    = true;
      cfilter["dead"]    = true;
      cfilter["deleted"] = true;
      cfilter["parent"]  = true;  // needed to look up story context

      DynamicJsonDocument cdoc(2048);
      err = deserializeJson(cdoc, cc,
                            DeserializationOption::Filter(cfilter),
                            DeserializationOption::NestingLimit(5));
      cc.stop();
      if (err) continue;

      const char* type = cdoc["type"] | "";
      if (strcmp(type, "comment") != 0) continue;
      if (cdoc["dead"]    | false) continue;
      if (cdoc["deleted"] | false) continue;
      const char* html   = cdoc["text"] | "";
      const char* author = cdoc["by"]   | "unknown";
      if (strlen(html) == 0) continue;

      LiveComment& lc = hnLiveComments[hnLiveCount];
      memset(&lc, 0, sizeof(lc));
      strncpy(lc.author, author, sizeof(lc.author) - 1);
      stripHtml(html, lc.text, sizeof(lc.text));
      uint32_t created = cdoc["time"] | 0u;
      lc.age_minutes = (now > created && created > 0)
                       ? (int32_t)((now - created) / 60) : 0;

      // Step 3: fetch parent to get story context.
      // For top-level comments, parent IS the story → gives story_title + story_id.
      // For nested replies, parent is another comment → we skip story context.
      int32_t parentId = cdoc["parent"] | 0;
      if (parentId > 0) {
        char ppath[40];
        snprintf(ppath, sizeof(ppath), "/v0/item/%ld.json", (long)parentId);

        WiFiClientSecure pc;
        if (hn_firebase_get(pc, ppath) == 200) {
          DynamicJsonDocument pfilter(64);
          pfilter["type"]  = true;
          pfilter["title"] = true;

          DynamicJsonDocument pdoc(512);
          err = deserializeJson(pdoc, pc,
                                DeserializationOption::Filter(pfilter),
                                DeserializationOption::NestingLimit(3));
          pc.stop();
          if (!err) {
            const char* ptype = pdoc["type"] | "";
            if (strcmp(ptype, "story") == 0) {
              const char* ptitle = pdoc["title"] | "";
              strncpy(lc.story_title, ptitle,                  sizeof(lc.story_title) - 1);
              snprintf(lc.story_id,   sizeof(lc.story_id), "%ld", (long)parentId);
            }
          }
        } else {
          pc.stop();
        }
      }

      hnLiveCount++;
    }
  }

  Serial.printf("[HN] fetched %d live comments\n", hnLiveCount);
  return hnLiveCount;
}
