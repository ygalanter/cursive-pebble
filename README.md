# Cursive

A handwritten-journal watchface for **Pebble Time 2** (emery, 200×228 color).

Time, day, and date are rendered in the *Black Pen* cursive script on a yellow ruled-paper background. Tap the watch to flip to a "diary" page with five user-configurable lines that can show live health stats.

## Screens

**Time screen (default)**
- Large cursive `HH:MM`
- "Today is &lt;Day&gt;"
- Full date (e.g. "April 26, 2026")

**Data screen (tap to toggle)**
- 5 lines of free-form text with placeholders for live data
- Auto-returns to the time screen after a configurable timeout (optional)

## Shake to switch

A single wrist-shake or tap toggles between the time screen and the data screen. Taps are debounced to one per second so accidental movement doesn't double-flip.

## Placeholders

Use any of these inside a data line; they are substituted at render time:

| Placeholder      | Value                                                      |
|------------------|------------------------------------------------------------|
| `%steps%`        | Steps taken today                                          |
| `%calories%`     | Active calories burned today                               |
| `%distance%`     | Walked distance today (km or mi, per system units)         |
| `%heartrate%`    | Current heart rate (bpm)                                   |
| `%battery%`      | Battery charge (`N pct`)                                   |

Example template: `I walked %steps% steps today` → `I walked 7421 steps today`.

## Settings

Configurable from the Pebble phone app:

- **Data Lines 1–5** — text shown on the data screen, with placeholders above
- **Paper Color** — background (full Pebble 64-color palette)
- **Lines Color** — horizontal rule color
- **Text Color** — color of all text
- **Data Screen Timeout** — seconds before the data screen auto-returns to the time screen; blank or `0` disables auto-return (max 300)

Settings persist across reboots.

## Build

Requires the [Pebble SDK](https://developer.rebble.io/) and Node.js.

```bash
npm install
pebble build
```

The `.pbw` lands in `build/cursive.pbw` (or `build/cursive-pebble.pbw` depending on the directory name).

## Install

**Emulator:**
```bash
pebble install --emulator emery
```

**Device (over Wi-Fi via the Pebble phone app's developer connection):**
```bash
pebble install --phone <phone-ip>
```

## Project layout

```
.
├── package.json          # app metadata + font + message keys
├── wscript               # Pebble SDK build script
├── config/index.json     # Clay settings UI
├── src/
│   ├── c/main.c          # watchface code
│   └── pkjs/index.js     # phone-side settings handler
└── resources/
    └── fonts/
        └── Black Pen.ttf # cursive display font
```
