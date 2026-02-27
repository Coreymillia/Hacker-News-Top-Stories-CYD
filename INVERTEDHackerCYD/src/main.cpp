// HackerCYD — Hacker News top stories on the CYD (Cheap Yellow Display)
//
// MODE_FEED: Scrollable list of 15 stories — tap a story to see detail
//            Footer: left-third = scroll up, right-third = scroll down
// MODE_TOP:  Single story detail — body tap left/right = prev/next story
//            Footer center = back to FEED, header tap = back to FEED
// MODE_CMTS: Per-story live comments (most recent) — scrollable
//            Footer: <prev | TOP | AUTO/PAUSE | next>
// MODE_LIVE: Rolling global live comment feed — newest comments site-wide
//            Footer: <prev | FEED | AUTO/PAUSE | HOLD/FREE | next>
//            HOLD fetches 25 past comments and freezes re-fetch/auto-scroll

#include <Arduino.h>
#include <WiFi.h>
#include <time.h>

#include <Arduino_GFX_Library.h>
#include <XPT2046_Touchscreen.h>
#include <qrcode.h>
#include "Portal.h"
#include "HN.h"

// ---------------------------------------------------------------------------
// Display — CYD ILI9341 320x240 landscape
// ---------------------------------------------------------------------------
#define GFX_BL 21
Arduino_DataBus *bus = new Arduino_HWSPI(2/*DC*/, 15/*CS*/, 14/*SCK*/, 13/*MOSI*/, 12/*MISO*/);
Arduino_GFX    *gfx = new Arduino_ILI9341(bus, GFX_NOT_DEFINED, 1/*landscape*/);

// ---------------------------------------------------------------------------
// Touch — XPT2046 on VSPI
// ---------------------------------------------------------------------------
#define XPT2046_IRQ   36
#define XPT2046_MOSI  32
#define XPT2046_MISO  39
#define XPT2046_CLK   25
#define XPT2046_CS    33
#define TOUCH_DEBOUNCE 300

SPIClass touchSPI(VSPI);
XPT2046_Touchscreen ts(XPT2046_CS, XPT2046_IRQ);
static unsigned long lastTouchTime = 0;

// ---------------------------------------------------------------------------
// Layout constants (must sum to 240)
//   HEADER_H + STORIES_PER_PAGE * ROW_H + FOOTER_H = 20 + 192 + 28 = 240
// ---------------------------------------------------------------------------
#define HEADER_H         20
#define FOOTER_H         28
#define ROW_H            32
#define STORIES_PER_PAGE  6

// HN orange (#FF6600 → RGB565 0xFB20)
#define HN_ORANGE 0xFB20
// Dim gray for secondary info
#define DIM_GRAY  0x7BEF

// Color palette for multicolor mode (cycles per story/comment index)
static const uint16_t MULTI_PALETTE[6] = {
  0xFB20,  // HN orange
  0x07FF,  // cyan
  0xFFE0,  // yellow
  0x07E0,  // green
  0xF81F,  // magenta
  0xFFFF,  // white
};

// Returns the theme color for a given row/index based on saved color setting.
static uint16_t getThemeColor(int idx) {
  if (strcmp(hc_font_color, "green") == 0) return 0x07E0;
  if (strcmp(hc_font_color, "blue")  == 0) return 0x001F;
  if (strcmp(hc_font_color, "cyan")  == 0) return 0x07FF;
  if (strcmp(hc_font_color, "white") == 0) return 0xFFFF;
  if (strcmp(hc_font_color, "multi") == 0) return MULTI_PALETTE[((unsigned)idx) % 6];
  return HN_ORANGE;
}

// ---------------------------------------------------------------------------
// Modes
// ---------------------------------------------------------------------------
#define MODE_FEED 0
#define MODE_TOP  1
#define MODE_QR   2
#define MODE_CMTS 3
#define MODE_LIVE 4

// 10-minute auto-refresh
#define UPDATE_INTERVAL      (10UL * 60UL * 1000UL)
// BOOT button pin (GPIO0, active LOW)
#define BOOT_PIN             0
#define BOOT_LONG_MS         2000UL   // hold >= 2s = re-enter setup portal
// CMTS auto-scroll interval (default 30 s, can be tuned)
#define AUTOSCROLL_INTERVAL  (30UL * 1000UL)
// LIVE feed refresh interval (60 s) and auto-scroll same as CMTS
#define LIVE_REFRESH_INTERVAL (60UL * 1000UL)

// ---------------------------------------------------------------------------
// App state
// ---------------------------------------------------------------------------
static int           sc_mode       = MODE_FEED;
static int           sc_scroll     = 0;   // first visible story index in FEED
static int           sc_topIdx     = 0;   // current story in TOP
static int           sc_cmtIdx     = 0;   // current comment in CMTS
static int           sc_cmtLine    = 0;   // first visible line within current comment
static bool          sc_fetched    = false;
static unsigned long lastUpdate    = 0;
static bool          sc_autoScroll     = false;  // CMTS auto-scroll on/off
static unsigned long sc_autoScrollLast = 0;      // millis() when last auto-advance fired
// LIVE mode state
static int           sc_liveIdx        = 0;      // current comment in LIVE
static int           sc_liveLine       = 0;      // first visible line in LIVE comment
static bool          sc_liveAutoScroll = false;  // LIVE auto-scroll on/off
static unsigned long sc_liveAutoLast   = 0;      // millis() of last LIVE auto-advance
static bool          sc_liveHold       = false;  // HOLD: lock live feed to one story thread
static char          sc_liveHeldId[12] = "";     // story_id of the held conversation
static char          sc_liveHeldTitle[80] = "";  // story_title of the held conversation
static unsigned long sc_liveLastFetch  = 0;      // millis() of last LIVE fetch

// Line-wrap cache for comment display
#define MAX_CMT_LINES 80
#define CMT_CHARS     52   // chars per line at textSize 1 on 320px screen
static char cmtLines[MAX_CMT_LINES][CMT_CHARS + 1];
static int  cmtLineCount = 0;

static void buildCmtLines(const char* text, int charsPerLine) {
  cmtLineCount = 0;
  String s(text);
  s.replace("\n", " | ");  // render paragraph breaks as visual separator
  while (s.length() > 0 && cmtLineCount < MAX_CMT_LINES) {
    if ((int)s.length() <= charsPerLine) {
      strncpy(cmtLines[cmtLineCount++], s.c_str(), CMT_CHARS);
      break;
    }
    int cut = charsPerLine;
    while (cut > 0 && s.charAt(cut) != ' ') cut--;
    if (cut == 0) cut = charsPerLine;
    strncpy(cmtLines[cmtLineCount++], s.substring(0, cut).c_str(), CMT_CHARS);
    s = s.substring(cut);
    s.trim();
  }
}

// ---------------------------------------------------------------------------
// Status banner (full-width, replaces header during loading/errors)
// ---------------------------------------------------------------------------
void showStatus(const char* msg) {
  gfx->fillRect(0, 0, gfx->width(), HEADER_H, RGB565_BLACK);
  gfx->setTextColor(RGB565_WHITE);
  gfx->setTextSize(1);
  gfx->setCursor(4, 6);
  gfx->print(msg);
  Serial.println(msg);
}

// ---------------------------------------------------------------------------
// Word-wrap helper — draws text at (x, y) wrapping at maxChars per line.
// Returns the y coordinate after the last line printed.
// ---------------------------------------------------------------------------
static int drawWrapped(const char* text, int x, int y,
                       int maxChars, int lineH,
                       uint16_t color, uint8_t textSz) {
  gfx->setTextColor(color);
  gfx->setTextSize(textSz);
  String s(text);
  while (s.length() > 0) {
    if ((int)s.length() <= maxChars) {
      gfx->setCursor(x, y);
      gfx->print(s);
      y += lineH;
      break;
    }
    // Find last space within maxChars
    int cut = maxChars;
    while (cut > 0 && s.charAt(cut) != ' ') cut--;
    if (cut == 0) cut = maxChars;  // no space — hard cut
    gfx->setCursor(x, y);
    gfx->print(s.substring(0, cut));
    y += lineH;
    s = s.substring(cut);
    s.trim();
    if (y > gfx->height() - lineH) break;  // safety
  }
  return y;
}

// ---------------------------------------------------------------------------
// Fetch + refresh
// ---------------------------------------------------------------------------
void fetchStories() {
  showStatus("Fetching HN stories...");
  int r = hnFetch();
  sc_scroll  = 0;
  sc_topIdx  = 0;
  sc_fetched = true;
  lastUpdate = millis();

  char msg[48];
  if (r < 0) {
    if (hnLastErr[0])
      snprintf(msg, sizeof(msg), "HN err %d %s", hnLastHttpCode, hnLastErr);
    else
      snprintf(msg, sizeof(msg), "HN error HTTP %d", hnLastHttpCode);
  } else {
    snprintf(msg, sizeof(msg), "Loaded %d stories", hnCount);
  }
  showStatus(msg);
  delay(r < 0 ? 3000 : 500);
}

// ---------------------------------------------------------------------------
// MODE_FEED renderer
// ---------------------------------------------------------------------------
void renderFeed() {
  // Header
  gfx->fillRect(0, 0, gfx->width(), HEADER_H, 0x1082);
  gfx->setTextSize(1);
  gfx->setTextColor(HN_ORANGE);
  gfx->setCursor(4, 6);
  gfx->print("HN");
  gfx->setTextColor(RGB565_WHITE);
  gfx->setCursor(18, 6);
  gfx->print(" Hacker News");
  if (hnCount > 0) {
    char buf[20];
    snprintf(buf, sizeof(buf), "Top %d", hnCount);
    gfx->setTextColor(DIM_GRAY);
    gfx->setCursor(gfx->width() - (int)strlen(buf) * 6 - 4, 6);
    gfx->print(buf);
  }

  // Story area background
  gfx->fillRect(0, HEADER_H, gfx->width(),
                gfx->height() - HEADER_H - FOOTER_H, RGB565_BLACK);

  if (hnCount == 0) {
    gfx->setTextColor(DIM_GRAY);
    gfx->setTextSize(2);
    gfx->setCursor(30, 108);
    gfx->print("No stories found");
    gfx->setTextSize(1);
    gfx->setCursor(90, 134);
    gfx->print("Check WiFi");
  } else {
    for (int i = 0; i < STORIES_PER_PAGE; i++) {
      int idx = sc_scroll + i;
      if (idx >= hnCount) break;

      const Story &s = hnStories[idx];
      int y = HEADER_H + i * ROW_H;

      // Metadata: #N  ^pts  [cmts]  domain
      char meta[56];
      snprintf(meta, sizeof(meta), "#%d  ^%d  [%d]  %.20s",
               idx + 1, s.points, s.comments, s.domain);
      gfx->setTextSize(1);
      gfx->setTextColor(HN_ORANGE);
      gfx->setCursor(4, y + 2);
      gfx->print(meta);

      // Title (truncated to 53 chars — full width at textSize 1)
      char title[54];
      strncpy(title, s.title, 53);
      title[53] = '\0';
      gfx->setTextColor(getThemeColor(idx));
      gfx->setCursor(4, y + 13);
      gfx->print(title);

      // Row divider
      gfx->drawFastHLine(0, y + ROW_H - 1, gfx->width(), 0x1082);
    }
  }

  // Footer
  gfx->fillRect(0, gfx->height() - FOOTER_H, gfx->width(), FOOTER_H, 0x0841);
  gfx->setTextSize(1);
  gfx->setTextColor(DIM_GRAY);
  // Left: scroll up hint
  if (sc_scroll > 0) {
    gfx->setCursor(4, gfx->height() - 18);
    gfx->print("< up");
  }
  // Center: LIVE button
  gfx->setTextColor(0x07FF);  // cyan
  gfx->setCursor(gfx->width() / 2 - 12, gfx->height() - 18);
  gfx->print("LIVE");
  // Right: scroll down hint
  if (sc_scroll + STORIES_PER_PAGE < hnCount) {
    const char* dn = "down >";
    gfx->setCursor(gfx->width() - (int)strlen(dn) * 6 - 4, gfx->height() - 18);
    gfx->print(dn);
  }
  // Scroll position dots
  if (hnCount > STORIES_PER_PAGE) {
    int pages   = (hnCount + STORIES_PER_PAGE - 1) / STORIES_PER_PAGE;
    int curPage = sc_scroll / STORIES_PER_PAGE;
    int dotX    = gfx->width() / 2 - pages * 6;
    for (int d = 0; d < pages; d++) {
      uint16_t col = (d == curPage) ? HN_ORANGE : 0x2104;
      gfx->fillCircle(dotX + d * 12, gfx->height() - 7, 3, col);
    }
  }
}

// ---------------------------------------------------------------------------
// MODE_TOP renderer — single story detail
// ---------------------------------------------------------------------------
void renderTop() {
  gfx->fillScreen(RGB565_BLACK);

  if (hnCount == 0) {
    gfx->fillRect(0, 0, gfx->width(), HEADER_H, 0x1082);
    gfx->setTextColor(HN_ORANGE);
    gfx->setTextSize(1);
    gfx->setCursor(4, 6);
    gfx->print("HN  No stories loaded");
    return;
  }

  if (sc_topIdx >= hnCount) sc_topIdx = 0;
  const Story &s = hnStories[sc_topIdx];

  // Header: story position + score/comments
  gfx->fillRect(0, 0, gfx->width(), HEADER_H, 0x1082);
  gfx->setTextSize(1);
  gfx->setTextColor(HN_ORANGE);
  gfx->setCursor(4, 6);
  char hdr[28];
  snprintf(hdr, sizeof(hdr), "HN  #%d of %d", sc_topIdx + 1, hnCount);
  gfx->print(hdr);
  char stat[20];
  snprintf(stat, sizeof(stat), "^%d [%d]", s.points, s.comments);
  gfx->setTextColor(DIM_GRAY);
  gfx->setCursor(gfx->width() - (int)strlen(stat) * 6 - 4, 6);
  gfx->print(stat);

  // Title — textSize 2 (12×16 px), 26 chars per line
  int y = HEADER_H + 6;
  y = drawWrapped(s.title, 4, y, 26, 18, RGB565_WHITE, 2);
  y += 4;

  // Divider
  gfx->drawFastHLine(0, y, gfx->width(), HN_ORANGE);
  y += 8;

  // Author
  gfx->setTextSize(1);
  gfx->setTextColor(DIM_GRAY);
  gfx->setCursor(4, y);
  gfx->print("by ");
  gfx->setTextColor(0xFFE0);  // yellow for username
  gfx->print(s.author);
  y += 14;

  // Domain
  gfx->setTextColor(HN_ORANGE);
  gfx->setCursor(4, y);
  gfx->print(s.domain);
  y += 14;

  // Age
  gfx->setTextColor(DIM_GRAY);
  gfx->setCursor(4, y);
  if (s.age_hours == 0) {
    gfx->print("just now");
  } else if (s.age_hours < 24) {
    char b[24];
    snprintf(b, sizeof(b), "%dh ago", s.age_hours);
    gfx->print(b);
  } else {
    char b[24];
    snprintf(b, sizeof(b), "%dd ago", s.age_hours / 24);
    gfx->print(b);
  }

  // Navigation footer — 5 zones (64px each): prev | FEED | CMTS | QR | next
  gfx->fillRect(0, gfx->height() - FOOTER_H, gfx->width(), FOOTER_H, 0x0841);
  gfx->setTextSize(1);
  // prev
  if (sc_topIdx > 0) {
    gfx->setTextColor(DIM_GRAY);
    gfx->setCursor(4, gfx->height() - 18);
    gfx->print("<prev");
  }
  // FEED
  gfx->setTextColor(DIM_GRAY);
  gfx->setCursor(70, gfx->height() - 18);
  gfx->print("FEED");
  // CMTS — highlight since it's new
  gfx->setTextColor(0x07FF);  // cyan
  gfx->setCursor(132, gfx->height() - 18);
  gfx->print("CMTS");
  // QR box
  gfx->drawRect(196, gfx->height() - FOOTER_H + 3, 28, FOOTER_H - 6, HN_ORANGE);
  gfx->setTextColor(HN_ORANGE);
  gfx->setCursor(201, gfx->height() - 18);
  gfx->print("QR");
  // next
  if (sc_topIdx < hnCount - 1) {
    gfx->setTextColor(DIM_GRAY);
    gfx->setCursor(262, gfx->height() - 18);
    gfx->print("next>");
  }
}

// ---------------------------------------------------------------------------
// MODE_CMTS renderer — one comment at a time, scrollable
// Touch top half of body = scroll up, bottom half = scroll down
// Footer: < prev cmt | TOP | next cmt >
// ---------------------------------------------------------------------------
void renderCmts() {
  gfx->fillScreen(RGB565_BLACK);

  const Story &story = hnStories[sc_topIdx];

  // Header
  gfx->fillRect(0, 0, gfx->width(), HEADER_H, 0x1082);
  gfx->setTextSize(1);
  gfx->setTextColor(0x07FF);  // cyan for comments header
  gfx->setCursor(4, 6);
  char hdr[48];
  if (hnCommentCount == 0) {
    snprintf(hdr, sizeof(hdr), "HN  Comments — loading...");
  } else {
    snprintf(hdr, sizeof(hdr), "HN  Comment %d/%d", sc_cmtIdx + 1, hnCommentCount);
  }
  gfx->print(hdr);
  // Story title abbreviated on right
  char abbr[22];
  strncpy(abbr, story.title, 21);
  abbr[21] = '\0';
  gfx->setTextColor(DIM_GRAY);
  gfx->setCursor(gfx->width() - (int)strlen(abbr) * 6 - 4, 6);
  // (skip if too long — it'll overlap)
  if ((int)strlen(abbr) * 6 < 160) gfx->print(abbr);

  if (hnCommentCount == 0) {
    gfx->setTextColor(DIM_GRAY);
    gfx->setTextSize(2);
    gfx->setCursor(40, 110);
    gfx->print("No comments found");
  } else {
    if (sc_cmtIdx >= hnCommentCount) sc_cmtIdx = 0;
    const Comment &c = hnComments[sc_cmtIdx];

    // Rebuild line cache when needed
    uint8_t cmtTextSz    = hc_font_size;
    int     charsPerLine = (gfx->width() - 8) / (6 * cmtTextSz);
    int     lineH        = 9 * cmtTextSz;
    buildCmtLines(c.text, charsPerLine);

    // Author + age line
    int y = HEADER_H + 4;
    gfx->setTextSize(1);
    gfx->setTextColor(0xFFE0);  // yellow for author
    gfx->setCursor(4, y);
    gfx->print(c.author);
    char age[16];
    if (c.age_hours < 24)
      snprintf(age, sizeof(age), "  %dh ago", c.age_hours);
    else
      snprintf(age, sizeof(age), "  %dd ago", c.age_hours / 24);
    gfx->setTextColor(DIM_GRAY);
    gfx->print(age);
    y += 12;

    // Divider
    gfx->drawFastHLine(0, y, gfx->width(), 0x2104);
    y += 4;

    // Comment lines
    int bodyBottom = gfx->height() - FOOTER_H - 2;
    int visLines = (bodyBottom - y) / lineH;

    // Clamp scroll
    int maxScroll = cmtLineCount - visLines;
    if (maxScroll < 0) maxScroll = 0;
    if (sc_cmtLine > maxScroll) sc_cmtLine = maxScroll;

    gfx->setTextColor(getThemeColor(sc_cmtIdx));
    gfx->setTextSize(cmtTextSz);
    for (int i = 0; i < visLines; i++) {
      int li = sc_cmtLine + i;
      if (li >= cmtLineCount) break;
      gfx->setCursor(4, y + i * lineH);
      gfx->print(cmtLines[li]);
    }

    // Scroll indicator on right edge
    if (cmtLineCount > visLines) {
      int barH = bodyBottom - (HEADER_H + 18);
      int thumbH = max(8, barH * visLines / cmtLineCount);
      int thumbY = (HEADER_H + 18) + (barH - thumbH) * sc_cmtLine / max(1, maxScroll);
      gfx->drawFastVLine(gfx->width() - 3, HEADER_H + 18, barH, 0x2104);
      gfx->fillRect(gfx->width() - 4, thumbY, 4, thumbH, DIM_GRAY);
    }
  }

  // Footer — 4 zones (80px each): <prev | TOP | AUTO/PAUSE | next>
  gfx->fillRect(0, gfx->height() - FOOTER_H, gfx->width(), FOOTER_H, 0x0841);
  gfx->setTextSize(1);
  // Zone 0: prev
  if (sc_cmtIdx > 0) {
    gfx->setTextColor(DIM_GRAY);
    gfx->setCursor(4, gfx->height() - 18);
    gfx->print("<prev");
  }
  // Zone 1: TOP
  gfx->setTextColor(HN_ORANGE);
  gfx->setCursor(88, gfx->height() - 18);
  gfx->print("TOP");
  // Zone 2: AUTO toggle — lit up cyan when active, dim when off
  if (sc_autoScroll) {
    gfx->setTextColor(0x07FF);  // cyan = running
    gfx->setCursor(168, gfx->height() - 18);
    gfx->print("PAUSE");
  } else {
    gfx->setTextColor(DIM_GRAY);
    gfx->setCursor(172, gfx->height() - 18);
    gfx->print("AUTO");
  }
  // Zone 3: next
  if (sc_cmtIdx < hnCommentCount - 1) {
    gfx->setTextColor(DIM_GRAY);
    gfx->setCursor(gfx->width() - 40, gfx->height() - 18);
    gfx->print("next>");
  }
}

// ---------------------------------------------------------------------------
// MODE_QR renderer — QR code for the HN discussion page
// Scan with phone camera to open the article/discussion in a browser.
// ---------------------------------------------------------------------------
void renderQR() {
  if (sc_topIdx >= hnCount) sc_topIdx = 0;
  const Story &s = hnStories[sc_topIdx];

  // Build HN item URL — always fits in QR version 3/4 ECC_LOW
  char hnUrl[52];
  snprintf(hnUrl, sizeof(hnUrl), "https://news.ycombinator.com/item?id=%s", s.objectId);

  QRCode qrcode;
  // Version 4 (33×33 modules) handles up to 78 bytes at ECC_LOW —
  // enough for any HN URL with margin to spare.
  uint8_t qrcodeData[qrcode_getBufferSize(4)];
  qrcode_initText(&qrcode, qrcodeData, 4, ECC_LOW, hnUrl);

  // Scale to fill available height; white background so QR is readable
  int availH  = gfx->height() - HEADER_H - FOOTER_H - 18;  // 18 = URL text row
  int modSize = availH / qrcode.size;
  if (modSize < 4) modSize = 4;
  if (modSize > 7) modSize = 7;

  int qrPx    = qrcode.size * modSize;
  int offsetX = (gfx->width()  - qrPx) / 2;
  int offsetY = HEADER_H + 6;

  // White background behind QR (needs quiet zone)
  gfx->fillRect(offsetX - 4, offsetY - 4, qrPx + 8, qrPx + 8, RGB565_WHITE);

  for (uint8_t y = 0; y < qrcode.size; y++) {
    for (uint8_t x = 0; x < qrcode.size; x++) {
      uint16_t col = qrcode_getModule(&qrcode, x, y) ? RGB565_BLACK : RGB565_WHITE;
      gfx->fillRect(offsetX + x * modSize, offsetY + y * modSize,
                    modSize, modSize, col);
    }
  }

  // Header (drawn after QR so it doesn't get covered)
  gfx->fillRect(0, 0, gfx->width(), HEADER_H, 0x1082);
  gfx->setTextSize(1);
  gfx->setTextColor(HN_ORANGE);
  gfx->setCursor(4, 6);
  char hdr[32];
  snprintf(hdr, sizeof(hdr), "HN  #%d — Scan to open", sc_topIdx + 1);
  gfx->print(hdr);

  // URL hint below QR
  int textY = offsetY + qrPx + 10;
  gfx->setTextSize(1);
  gfx->setTextColor(DIM_GRAY);
  gfx->setCursor(4, textY);
  gfx->print("news.ycombinator.com/item?id=");
  gfx->setTextColor(RGB565_WHITE);
  gfx->print(s.objectId);

  // Footer
  gfx->fillRect(0, gfx->height() - FOOTER_H, gfx->width(), FOOTER_H, 0x0841);
  gfx->setTextSize(1);
  gfx->setTextColor(DIM_GRAY);
  gfx->setCursor(gfx->width() / 2 - 42, gfx->height() - 18);
  gfx->print("tap anywhere to go back");
}

// ---------------------------------------------------------------------------
// MODE_LIVE renderer — rolling global HN comment feed (newest first)
// ---------------------------------------------------------------------------
void renderLive() {
  gfx->fillScreen(RGB565_BLACK);

  // Header
  gfx->fillRect(0, 0, gfx->width(), HEADER_H, 0x1082);
  gfx->setTextSize(1);
  gfx->setTextColor(0x07FF);  // cyan = live
  gfx->setCursor(4, 6);
  if (sc_liveHold) {
    // Show which story thread is locked
    char hdr[36];
    snprintf(hdr, sizeof(hdr), "HN  HOLD %d/%d", sc_liveIdx + 1, hnLiveCount);
    gfx->print(hdr);
    // Story title on right (truncated)
    char abbr[22];
    strncpy(abbr, sc_liveHeldTitle, 21); abbr[21] = '\0';
    gfx->setTextColor(0xFFE0);  // yellow = locked story
    int tw = strlen(abbr) * 6;
    if (tw < 190) {
      gfx->setCursor(gfx->width() - tw - 4, 6);
      gfx->print(abbr);
    }
  } else if (hnLiveCount == 0) {
    gfx->print("HN  LIVE — loading...");
  } else {
    char hdr[32];
    snprintf(hdr, sizeof(hdr), "HN  LIVE  %d/%d", sc_liveIdx + 1, hnLiveCount);
    gfx->print(hdr);
  }
  // Live pulse dot (red = global, yellow = locked to story)
  gfx->fillCircle(gfx->width() - 8, HEADER_H / 2, 4, sc_liveHold ? 0xFFE0 : 0xF800);

  if (hnLiveCount == 0) {
    gfx->setTextColor(DIM_GRAY);
    gfx->setTextSize(1);
    gfx->setCursor(40, 110);
    gfx->print("Fetching live comments...");
  } else {
    if (sc_liveIdx >= hnLiveCount) sc_liveIdx = 0;
    const LiveComment &lc = hnLiveComments[sc_liveIdx];

    uint8_t liveSz    = hc_font_size;
    int charsPerLine  = (gfx->width() - 8) / (6 * liveSz);
    int lineH         = 9 * liveSz;
    buildCmtLines(lc.text, charsPerLine);

    int y = HEADER_H + 4;

    // Author (yellow) + age
    gfx->setTextSize(1);
    gfx->setTextColor(0xFFE0);
    gfx->setCursor(4, y);
    gfx->print(lc.author);
    char age[20];
    if (lc.age_minutes < 1)        snprintf(age, sizeof(age), "  just now");
    else if (lc.age_minutes < 60)  snprintf(age, sizeof(age), "  %dm ago", lc.age_minutes);
    else                           snprintf(age, sizeof(age), "  %dh ago", lc.age_minutes / 60);
    gfx->setTextColor(DIM_GRAY);
    gfx->print(age);
    y += 12;

    // Story title — hidden when HOLDing (it's already shown in the header)
    if (!sc_liveHold) {
      char stitle[48];
      strncpy(stitle, lc.story_title, 47); stitle[47] = '\0';
      gfx->setTextColor(0x07FF);
      gfx->setCursor(4, y);
      gfx->print("on: ");
      gfx->setTextColor(DIM_GRAY);
      gfx->print(stitle);
      y += 12;
    }

    // Divider
    gfx->drawFastHLine(0, y, gfx->width(), 0x2104);
    y += 4;

    // Comment lines
    int bodyBottom = gfx->height() - FOOTER_H - 2;
    int visLines   = (bodyBottom - y) / lineH;
    int maxScroll  = cmtLineCount - visLines;
    if (maxScroll < 0) maxScroll = 0;
    if (sc_liveLine > maxScroll) sc_liveLine = maxScroll;

    gfx->setTextColor(getThemeColor(sc_liveIdx));
    gfx->setTextSize(liveSz);
    for (int i = 0; i < visLines; i++) {
      int li = sc_liveLine + i;
      if (li >= cmtLineCount) break;
      gfx->setCursor(4, y + i * lineH);
      gfx->print(cmtLines[li]);
    }

    // Scroll indicator
    if (cmtLineCount > visLines) {
      int barH   = bodyBottom - (HEADER_H + 30);
      int thumbH = max(8, barH * visLines / cmtLineCount);
      int thumbY = (HEADER_H + 30) + (barH - thumbH) * sc_liveLine / max(1, maxScroll);
      gfx->drawFastVLine(gfx->width() - 3, HEADER_H + 30, barH, 0x2104);
      gfx->fillRect(gfx->width() - 4, thumbY, 4, thumbH, DIM_GRAY);
    }
  }

  // Footer — 5 zones (64px each): <prev | FEED | AUTO/PAUSE | HOLD/FREE | next>
  gfx->fillRect(0, gfx->height() - FOOTER_H, gfx->width(), FOOTER_H, 0x0841);
  gfx->setTextSize(1);
  // Zone 0 (0-63): prev
  if (sc_liveIdx > 0) {
    gfx->setTextColor(DIM_GRAY);
    gfx->setCursor(4, gfx->height() - 18);
    gfx->print("<prev");
  }
  // Zone 1 (64-127): FEED
  gfx->setTextColor(HN_ORANGE);
  gfx->setCursor(76, gfx->height() - 18);
  gfx->print("FEED");
  // Zone 2 (128-191): AUTO/PAUSE
  if (sc_liveAutoScroll) {
    gfx->setTextColor(0x07FF);
    gfx->setCursor(136, gfx->height() - 18);
    gfx->print("PAUSE");
  } else {
    gfx->setTextColor(DIM_GRAY);
    gfx->setCursor(140, gfx->height() - 18);
    gfx->print("AUTO");
  }
  // Zone 3 (192-255): HOLD / FREE
  if (sc_liveHold) {
    gfx->setTextColor(0xFFE0);  // yellow = held
    gfx->setCursor(206, gfx->height() - 18);
    gfx->print("FREE");
  } else {
    gfx->setTextColor(DIM_GRAY);
    gfx->setCursor(206, gfx->height() - 18);
    gfx->print("HOLD");
  }
  // Zone 4 (256-319): next
  if (sc_liveIdx < hnLiveCount - 1) {
    gfx->setTextColor(DIM_GRAY);
    gfx->setCursor(270, gfx->height() - 18);
    gfx->print("next>");
  }
}

// ---------------------------------------------------------------------------
// Dispatch render
// ---------------------------------------------------------------------------
void render() {
  if      (sc_mode == MODE_FEED) renderFeed();
  else if (sc_mode == MODE_TOP)  renderTop();
  else if (sc_mode == MODE_CMTS) renderCmts();
  else if (sc_mode == MODE_LIVE) renderLive();
  else                           renderQR();
}

// ---------------------------------------------------------------------------
// Touch handler
// ---------------------------------------------------------------------------
void handleTouch(int tx, int ty) {
  const int footerY = gfx->height() - FOOTER_H;

  // QR mode: any tap goes back to TOP
  if (sc_mode == MODE_QR) {
    sc_mode = MODE_TOP;
    renderTop();
    return;
  }

  // CMTS mode
  if (sc_mode == MODE_CMTS) {
    if (ty >= footerY) {
      // Footer 4 zones (80px each): <prev | TOP | AUTO/PAUSE | next>
      if (tx < 80) {
        if (sc_cmtIdx > 0) {
          sc_cmtIdx--; sc_cmtLine = 0;
          sc_autoScrollLast = millis();  // reset timer on manual nav
          renderCmts();
        }
      } else if (tx < 160) {
        sc_mode = MODE_TOP;
        renderTop();
      } else if (tx < 240) {
        // Toggle auto-scroll
        sc_autoScroll = !sc_autoScroll;
        sc_autoScrollLast = millis();  // start timer from now
        renderCmts();
      } else {
        if (sc_cmtIdx < hnCommentCount - 1) {
          sc_cmtIdx++; sc_cmtLine = 0;
          sc_autoScrollLast = millis();  // reset timer on manual nav
          renderCmts();
        }
      }
    } else if (ty >= HEADER_H) {
      // Body: top half = scroll up, bottom half = scroll down
      int bodyMid = HEADER_H + (footerY - HEADER_H) / 2;
      if (ty < bodyMid) {
        if (sc_cmtLine > 0) { sc_cmtLine--; renderCmts(); }
      } else {
        int visLines = (footerY - HEADER_H - 18) / (9 * hc_font_size);
        if (sc_cmtLine + visLines < cmtLineCount) { sc_cmtLine++; renderCmts(); }
      }
    }
    return;
  }

  const int thirdW  = gfx->width() / 3;

  if (sc_mode == MODE_FEED) {
    if (ty >= footerY) {
      if (tx < thirdW) {
        if (sc_scroll > 0) { sc_scroll--; renderFeed(); }
      } else if (tx > 2 * thirdW) {
        if (sc_scroll + STORIES_PER_PAGE < hnCount) { sc_scroll++; renderFeed(); }
      } else {
        // Center footer: enter LIVE mode
        sc_mode    = MODE_LIVE;
        sc_liveIdx = 0;
        sc_liveLine = 0;
        sc_liveHold = false;
        sc_liveHeldId[0]    = '\0';
        sc_liveHeldTitle[0] = '\0';
        showStatus("Fetching live comments...");
        hnFetchLive();
        sc_liveLastFetch = millis();
        sc_liveAutoLast  = millis();
        renderLive();
      }
    } else if (ty >= HEADER_H) {
      int row = (ty - HEADER_H) / ROW_H;
      int idx = sc_scroll + row;
      if (idx >= 0 && idx < hnCount) {
        sc_topIdx = idx;
        sc_mode   = MODE_TOP;
        renderTop();
      }
    }
  } else if (sc_mode == MODE_LIVE) {
    if (ty >= footerY) {
      // Footer 5 zones (64px each): <prev | FEED | AUTO/PAUSE | HOLD/FREE | next>
      if (tx < 64) {
        if (sc_liveIdx > 0) { sc_liveIdx--; sc_liveLine = 0; sc_liveAutoLast = millis(); renderLive(); }
      } else if (tx < 128) {
        sc_liveHold = false;
        sc_liveHeldId[0]    = '\0';
        sc_liveHeldTitle[0] = '\0';
        sc_mode = MODE_FEED;
        renderFeed();
      } else if (tx < 192) {
        sc_liveAutoScroll = !sc_liveAutoScroll;
        sc_liveAutoLast   = millis();
        renderLive();
      } else if (tx < 256) {
        // Toggle HOLD: lock onto current comment's story thread, or resume global
        sc_liveHold = !sc_liveHold;
        if (sc_liveHold && hnLiveCount > 0) {
          // Record which story to lock onto
          strncpy(sc_liveHeldId,    hnLiveComments[sc_liveIdx].story_id,    sizeof(sc_liveHeldId)    - 1);
          strncpy(sc_liveHeldTitle, hnLiveComments[sc_liveIdx].story_title, sizeof(sc_liveHeldTitle) - 1);
          // Fetch 25 most recent comments for this story
          showStatus("Loading conversation...");
          hnFetchLive(25, sc_liveHeldId);
          sc_liveLastFetch = millis();
          sc_liveIdx  = 0;
          sc_liveLine = 0;
        } else {
          // FREE — resume global live feed
          sc_liveHeldId[0]    = '\0';
          sc_liveHeldTitle[0] = '\0';
          showStatus("Resuming live feed...");
          hnFetchLive(8);
          sc_liveLastFetch = millis();
          sc_liveIdx  = 0;
          sc_liveLine = 0;
        }
        renderLive();
      } else {
        if (sc_liveIdx < hnLiveCount - 1) { sc_liveIdx++; sc_liveLine = 0; sc_liveAutoLast = millis(); renderLive(); }
      }
    } else if (ty >= HEADER_H) {
      int bodyMid = HEADER_H + (footerY - HEADER_H) / 2;
      if (ty < bodyMid) {
        if (sc_liveLine > 0) { sc_liveLine--; renderLive(); }
      } else {
        int visLines = (footerY - HEADER_H - 30) / (9 * hc_font_size);
        if (sc_liveLine + visLines < cmtLineCount) { sc_liveLine++; renderLive(); }
      }
    }
  } else {
    // MODE_TOP
    if (ty < HEADER_H) {
      sc_mode = MODE_FEED;
      renderFeed();
    } else if (ty >= footerY) {
      // Footer 5 zones (64px each): 0-63=prev, 64-127=FEED, 128-191=CMTS, 192-255=QR, 256-319=next
      if (tx < 64) {
        if (sc_topIdx > 0) { sc_topIdx--; renderTop(); }
      } else if (tx < 128) {
        sc_mode = MODE_FEED;
        renderFeed();
      } else if (tx < 192) {
        // CMTS — fetch if not already loaded for this story
        if (strcmp(hnCmtStoryId, hnStories[sc_topIdx].objectId) != 0) {
          showStatus("Fetching comments...");
          gfx->fillRect(0, HEADER_H, gfx->width(), gfx->height() - HEADER_H, RGB565_BLACK);
          hnFetchComments(hnStories[sc_topIdx].objectId);
        }
        sc_cmtIdx  = 0;
        sc_cmtLine = 0;
        sc_mode    = MODE_CMTS;
        renderCmts();
      } else if (tx < 256) {
        sc_mode = MODE_QR;
        renderQR();
      } else {
        if (sc_topIdx < hnCount - 1) { sc_topIdx++; renderTop(); }
      }
    } else {
      if (tx < gfx->width() / 2) {
        if (sc_topIdx > 0) { sc_topIdx--; renderTop(); }
      } else {
        if (sc_topIdx < hnCount - 1) { sc_topIdx++; renderTop(); }
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);

  // Backlight on
  pinMode(GFX_BL, OUTPUT);
  digitalWrite(GFX_BL, HIGH);

  // Display init
  gfx->begin();
  gfx->invertDisplay(true);  // Fix for CYDs with inverted display hardware
  gfx->fillScreen(RGB565_BLACK);

  // Touch init
  touchSPI.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
  ts.begin(touchSPI);
  ts.setRotation(1);

  // BOOT button
  pinMode(BOOT_PIN, INPUT_PULLUP);

  // Load saved settings; give user 3 s to hold BOOT and re-enter portal
  hcLoadSettings();
  bool showPortal = !hc_has_settings;
  if (!showPortal) {
    showStatus("Hold BOOT to change settings...");
    for (int i = 0; i < 30 && !showPortal; i++) {
      if (digitalRead(BOOT_PIN) == LOW) showPortal = true;
      delay(100);
    }
  }
  if (showPortal) {
    hcInitPortal();
    while (!portalDone) hcRunPortal();
    hcClosePortal();
  }

  // Connect to WiFi
  showStatus("Connecting to WiFi...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(hc_wifi_ssid, hc_wifi_pass);
  uint32_t wt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wt < 15000) {
    delay(300);
  }
  if (WiFi.status() != WL_CONNECTED) {
    showStatus("WiFi failed - re-running setup");
    delay(2000);
    hcInitPortal();
    while (!portalDone) hcRunPortal();
    hcClosePortal();
    ESP.restart();
  }
  showStatus("WiFi connected!");
  delay(400);

  // NTP sync (needed for age_hours calculation)
  configTime(0, 0, "pool.ntp.org");
  uint32_t ntpStart = millis();
  while (time(nullptr) < 946684800UL && millis() - ntpStart < 5000) delay(100);

  // Initial fetch
  fetchStories();
  render();
}

// ---------------------------------------------------------------------------
// Loop
// ---------------------------------------------------------------------------
void loop() {
  // Auto-refresh every 10 minutes
  if (millis() - lastUpdate > UPDATE_INTERVAL) {
    fetchStories();
    render();
  }

  // CMTS auto-scroll: advance to next comment every AUTOSCROLL_INTERVAL
  if (sc_mode == MODE_CMTS && sc_autoScroll && hnCommentCount > 0) {
    if (millis() - sc_autoScrollLast >= AUTOSCROLL_INTERVAL) {
      sc_autoScrollLast = millis();
      sc_cmtLine = 0;
      sc_cmtIdx  = (sc_cmtIdx + 1) % hnCommentCount;  // wrap at end
      renderCmts();
    }
  }

  // LIVE mode: periodic re-fetch + auto-scroll
  if (sc_mode == MODE_LIVE) {
    // Re-fetch every 60 seconds — global feed OR locked story thread if HOLDing
    if (sc_liveLastFetch == 0 || millis() - sc_liveLastFetch >= LIVE_REFRESH_INTERVAL) {
      if (sc_liveHold) {
        // Stay locked: fetch newest comments for this story only
        hnFetchLive(25, sc_liveHeldId);
      } else {
        hnFetchLive();
      }
      sc_liveLastFetch = millis();
      if (!sc_liveHold) {
        // Global mode: auto-scroll or clamp
        if (sc_liveAutoScroll) {
          sc_liveIdx  = 0;
          sc_liveLine = 0;
          sc_liveAutoLast = millis();
        } else {
          if (sc_liveIdx >= hnLiveCount) sc_liveIdx = 0;
        }
      } else {
        // Held mode: clamp in case new comments pushed count around
        if (sc_liveIdx >= hnLiveCount) sc_liveIdx = 0;
      }
      renderLive();
    }
    // Auto-scroll only runs in global (non-held) mode
    if (!sc_liveHold && sc_liveAutoScroll && hnLiveCount > 0) {
      if (millis() - sc_liveAutoLast >= AUTOSCROLL_INTERVAL) {
        sc_liveAutoLast = millis();
        sc_liveLine = 0;
        sc_liveIdx  = (sc_liveIdx + 1) % hnLiveCount;
        renderLive();
      }
    }
  }

  // BOOT button: short press = refresh feed, long press = re-enter setup portal
  if (digitalRead(BOOT_PIN) == LOW) {
    delay(50);  // debounce
    if (digitalRead(BOOT_PIN) == LOW) {
      unsigned long pressStart = millis();
      while (digitalRead(BOOT_PIN) == LOW) delay(10);
      unsigned long held = millis() - pressStart;

      if (held >= BOOT_LONG_MS) {
        // Long press — re-enter captive portal then restart
        showStatus("Entering setup...");
        delay(500);
        portalDone = false;
        hcInitPortal();
        while (!portalDone) hcRunPortal();
        hcClosePortal();
        ESP.restart();
      } else {
        // Short press — force refresh feed
        fetchStories();
        sc_mode = MODE_FEED;
        render();
      }
    }
  }

  // Touch
  if (ts.tirqTouched() && ts.touched()) {
    TS_Point p = ts.getPoint();
    unsigned long now = millis();
    if (now - lastTouchTime > TOUCH_DEBOUNCE) {
      lastTouchTime = now;
      // Map raw ADC values to screen coordinates (landscape)
      int tx = map(p.x, 200, 3900, 0, gfx->width());
      int ty = map(p.y, 240, 3900, 0, gfx->height());
      tx = constrain(tx, 0, gfx->width()  - 1);
      ty = constrain(ty, 0, gfx->height() - 1);
      handleTouch(tx, ty);
    }
  }

  delay(10);
}
