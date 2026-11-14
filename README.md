# It's Automated

Control a WiZ smart lamp and run routines from your own web app, with no Alexa, no hub and no cloud account. A ₹500 ESP32 board sits on your Wi-Fi as an always-on bridge: it serves the app to your phone, talks to the lamp over WiZ's local network API, and fires the routines itself.

```
 Phone (home-screen web app)  ──HTTP──▶  ESP32 bridge  ──UDP 38899──▶  WiZ lamp
                                         schedules, sunrise/sunset,
                                         fades, sleep timer
```

## What it does

- **Lamp control**: power, brightness, warm-to-cool white, colours, built-in WiZ scenes.
- **My scenes**: save the lamp's current look (for example "Reading") and recall it in one tap.
- **Routines**: at a fixed time, or at sunrise / sunset with an offset, on chosen weekdays.
- **Fades**: wake-up light that brightens over N minutes, bedtime routine that dims and switches off.
- **Sleep timer**: dim slowly and switch off after 15 to 90 minutes.
- **Several lamps**: finds every WiZ light on the network; control one or all.

Routines run on the ESP32, so they fire even when your phone is away or off.

## What you need

| Item | Notes |
|---|---|
| ESP32 dev board | Any "ESP32 DevKit / NodeMCU-32S" 30-pin or 38-pin board (about ₹400 to ₹550) |
| Micro-USB **data** cable | Charge-only cables will not flash |
| USB phone charger | To power the board permanently, anywhere in Wi-Fi range |
| A computer, once | Only for the first flash |
| WiZ light | Already set up in the WiZ app, on 2.4 GHz Wi-Fi |

In the WiZ app, make sure **Settings → Security → Allow local communication** is switched on. Without it the lamp ignores local commands.

## Install

### Option A: one-click web flasher

1. On a laptop, open **https://builtonleverage.github.io/its-automated/** in Chrome or Edge.
2. Plug in the ESP32, click **Install**, choose the serial port.

(The page is published by the GitHub Actions workflow in this repo. If you fork it, enable *Settings → Pages → Source: GitHub Actions*.)

### Option B: PlatformIO

```bash
pip install platformio
pio run -t upload
```

## First-time setup

1. Power the ESP32. It creates a Wi-Fi hotspot called **Its-Automated-Setup**.
2. Join it from your phone, pick your home Wi-Fi in the page that pops up, and enter the password.
3. Back on home Wi-Fi, open **http://its-automated.local** (or the IP address your router shows for it).
4. The lamp should appear by itself. If not: Settings → **Find lamps**, or **Add by IP**.
5. Set your city in Settings so sunrise and sunset are right.
6. iPhone: Share → **Add to Home Screen**. Android: menu → **Add to Home screen**.

The blue LED on the board is lit while it is connected to Wi-Fi.

## Good to know

- **Home Wi-Fi only.** The app works when your phone is on the same network as the bridge. Nothing is exposed to the internet.
- **No login.** Anyone on your Wi-Fi can open the app. Do not port-forward it.
- **Plain HTTP.** The app is served from the device, so it installs to the home screen but has no offline mode or push notifications (those need HTTPS).
- **Clock.** The bridge gets the time from the internet (NTP). Routines pause until the clock is set. Daylight saving is not handled; the UTC offset is a fixed number.
- **Power cuts.** If the lamp is switched off at the wall, nothing can reach it. A routine missed while the bridge was off is skipped, not replayed.
- **Manual changes during a fade** cancel the fade.

## Develop without hardware

```bash
node tools/mock-server.js     # http://localhost:8080 with a pretend lamp
g++ -std=c++17 -o /tmp/test_sun test/test_sun.cpp && /tmp/test_sun
```

Project layout:

```
src/main.cpp          firmware: Wi-Fi setup, WiZ UDP, scheduler, fades, HTTP API
src/sun.h             sunrise / sunset maths (no Arduino dependencies)
web/                  the web app (one HTML file, no build step)
scripts/embed_web.py  gzips web/ into the firmware at build time
tools/mock-server.js  fake bridge for UI work
flasher/              web flasher page published to GitHub Pages
```

## HTTP API

All bodies are JSON. `target` is `"all"` or a lamp MAC.

| Method and path | Body | Purpose |
|---|---|---|
| `GET /api/state` | | Everything: lamps, routines, scenes, settings, time, sun times |
| `POST /api/control` | `{target, action}` | Apply an action now |
| `POST /api/sleep` | `{target, minutes}` | Fade to off; `0` cancels |
| `POST /api/routines` | `{routines: [...]}` | Replace the routine list |
| `POST /api/routines/run` | `{id}` | Run one routine now |
| `POST /api/presets` | `{presets: [...]}` | Replace saved scenes |
| `POST /api/discover` | `{}` | Search the network for WiZ lights |
| `POST /api/lamps/add` \| `update` \| `remove` | `{ip}` \| `{mac, name}` \| `{mac}` | Manage lamps |
| `POST /api/settings` | `{city, lat, lon, tzOffsetMin}` | Location and time zone |
| `POST /api/wifi/reset` | `{}` | Forget Wi-Fi and reopen the setup hotspot |

An action looks like:

```json
{ "power": true, "dimming": 80, "temp": 2700, "fadeMin": 15 }
```

Use `temp` (2200 to 6500 K), or `r`/`g`/`b`, or `sceneId` (WiZ scene number). `fadeMin` is optional.

A routine looks like:

```json
{
  "id": "r1", "name": "On at sunset", "enabled": true,
  "trigger": { "type": "sunset", "offset": -15 },
  "days": [0, 1, 2, 3, 4, 5, 6],
  "target": "all",
  "action": { "power": true, "dimming": 80, "temp": 2700 }
}
```

`trigger.type` is `time` (with `"time": "HH:MM"`), `sunrise` or `sunset` (with `offset` in minutes). Days are 0 = Sunday to 6 = Saturday.

## Status

Early version. The web app is tested end to end against the mock bridge and the firmware is built in CI, but it has had little time on real hardware yet. Issues and pull requests are welcome.

Not affiliated with WiZ or Signify.

## License

MIT
