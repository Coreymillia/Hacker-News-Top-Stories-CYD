# 🟠 HackerCYD

**Hacker News reader for the CYD — Cheap Yellow Display (ESP32 + ILI9341 320×240 touchscreen)**

HackerCYD fetches the top 15 Hacker News front-page stories and displays them on a touch-enabled ESP32 display. Browse stories, read comment threads, scan QR codes to open discussions on your phone, and watch a live rolling feed of the newest comments being posted site-wide — all from a device that fits on a desk and costs about $10.

---

## Photos

| Comments — multicolor theme | More comments |
|---|---|
| ![Comment 2/8 — peteforde on LLMs as exoskeletons](IMG_20260226_200149.jpg) | ![Comment 3/8 — paulryanrogers](IMG_20260226_200318.jpg) |

| Large font mode | Large font — pink/magenta cycle |
|---|---|
| ![Comment 1/8 — large font, orange](IMG_20260226_200513.jpg) | ![Comment 5/8 — large font, magenta](IMG_20260226_200627.jpg) |

| QR code mode — scan to open in browser |
|---|
| ![QR code for Will vibe coding end — news.ycombinator.com/item?id=47167931](IMG_20260226_200702.jpg) |

---

## Modes

| Mode | Description |
|------|-------------|
| **FEED** | Scrollable list of up to 15 front-page stories with score, comment count, domain, and age |
| **TOP** | Single story detail view — author, age, domain, navigate prev/next |
| **CMTS** | Per-story comment reader — up to 8 top comments, scrollable, with author and age |
| **QR** | Scannable QR code linking directly to the HN discussion page |
| **LIVE** | Rolling global feed of the newest comments being posted site-wide — auto-scrollable, with HOLD to lock onto a single story thread |

---

## Features

- 📰 **15 top HN stories** fetched live, auto-refreshed every 10 minutes
- 💬 **Comment reader** — up to 8 top comments per story with scrolling, author, and age
- 🔴 **Live comment feed** — newest comments site-wide, refreshed every 60 seconds; HOLD locks to a single story thread; AUTO scrolls automatically
- 📱 **QR code** for every story — scan to open the full HN thread in a browser
- 🎨 **Font color themes** — Orange, Green, Blue, Cyan, White, or **Multicolor** (cycles through a palette per story/comment)
- 🔡 **Font size** — Small, Medium, or Large
- 🌐 **Captive portal setup** — first boot opens a WiFi AP at `192.168.4.1` to configure credentials and display preferences
- 🔁 **BOOT button** — short press = instant story refresh; long press (≥ 2s) = re-enter setup portal
- 💾 **All settings persist to NVS flash** — WiFi, color theme, font size, and last active mode (FEED or LIVE) survive power cycles

---

## Hardware

- **CYD** — "Cheap Yellow Display" — ESP32 dev board with built-in ILI9341 320×240 TFT and XPT2046 touch controller
- Any CYD variant with the standard pinout will work

### Wiring (standard CYD — no changes needed)

| Function | GPIO |
|----------|------|
| TFT DC | 2 |
| TFT CS | 15 |
| TFT SCK | 14 |
| TFT MOSI | 13 |
| TFT MISO | 12 |
| Backlight | 21 |
| Touch CS | 33 |
| Touch IRQ | 36 |
| Touch CLK | 25 |
| Touch MOSI | 32 |
| Touch MISO | 39 |

---

## Building & Flashing

Requires [PlatformIO](https://platformio.org/).

```bash
git clone https://github.com/YOUR_USERNAME/HackerCYD
cd HackerCYD
pio run --target upload
pio device monitor
```

---

## First Boot Setup

1. Power on the device
2. It broadcasts a WiFi AP called **`HackerCYD_Setup`**
3. Connect to it and open **`http://192.168.4.1`** in a browser
4. Enter your WiFi credentials, choose a font color theme and size, tap **Save & Connect**
5. The device connects, fetches stories, and you're reading HN

**To re-enter setup later:** hold the **BOOT button** during the first 3 seconds of startup, or hold it for 2+ seconds while running.

---

## Navigation

### FEED mode
| Touch zone | Action |
|---|---|
| Left footer third | Scroll stories up |
| Right footer third | Scroll stories down |
| Center footer | Enter LIVE comment feed |
| Story row | Open story detail (TOP mode) |

### TOP mode
| Touch zone | Action |
|---|---|
| Header | Back to FEED |
| Body left half | Previous story |
| Body right half | Next story |
| Footer — FEED | Back to FEED |
| Footer — CMTS | Load & view comments |
| Footer — QR | Show QR code |
| Footer — prev/next | Navigate stories |

### CMTS mode
| Touch zone | Action |
|---|---|
| Body top half | Scroll comment text up |
| Body bottom half | Scroll comment text down |
| Footer left | Previous comment |
| Footer center (TOP) | Back to story detail |
| Footer center (AUTO/PAUSE) | Toggle auto-scroll |
| Footer right | Next comment |

### LIVE mode
| Touch zone | Action |
|---|---|
| Footer — `<` | Previous comment |
| Footer — FEED | Back to story feed |
| Footer — AUTO/PAUSE | Toggle auto-scroll |
| Footer — HOLD/FREE | Lock to / release current story thread |
| Footer — `>` | Next comment |
| Body top half | Scroll comment text up |
| Body bottom half | Scroll comment text down |

### BOOT button (anytime)
| Press | Action |
|---|---|
| Short press (< 2s) | Force refresh — re-fetches all stories |
| Long press (≥ 2s) | Re-enter setup portal |

---

## Dependencies

- [Arduino GFX Library](https://github.com/moononournation/Arduino_GFX) — display driver
- [XPT2046_Touchscreen](https://github.com/PaulStoffregen/XPT2046_Touchscreen) — touch input
- [ArduinoJson](https://arduinojson.org/) — JSON parsing
- [QRCode](https://github.com/ricmoo/QRCode) — QR code generation

All managed automatically by PlatformIO via `platformio.ini`.

---

## API

Data is fetched from the [official Hacker News Firebase API](https://github.com/HackerNews/API) — no API key required, no rate limits, maintained by Y Combinator.

| Endpoint | Used for |
|---|---|
| `hacker-news.firebaseio.com/v0/topstories.json` | Ranked list of top story IDs |
| `hacker-news.firebaseio.com/v0/item/{id}.json` | Story and comment details |
| `hacker-news.firebaseio.com/v0/maxitem.json` | Highest item ID (used to walk newest comments for LIVE feed) |

---

## License

MIT. Do whatever you want with it.

