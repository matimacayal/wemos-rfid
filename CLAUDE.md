# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

Single-file Arduino sketch (`WemosRFID.ino`) for a **WeMos D1 Mini (ESP8266)** + **MFRC522 (RC522)** RFID reader. The board serves a self-contained web UI for reading/cloning/writing 13.56 MHz cards. There is no separate frontend build, no SD card, no filesystem upload — the UI is a PROGMEM string literal compiled into the firmware.

The device runs in one of two network modes (`NetMode` enum):
- `NET_SETUP_AP` — first boot, no creds, or saved creds failed. Hosts the `WemosRFID` hotspot in `WIFI_AP_STA` (STA leg kept alive only so `WiFi.scanNetworks()` works during provisioning).
- `NET_STATION` — joined the saved external Wi-Fi from `/wifi.txt` (line 1 SSID, line 2 password). On STA failure within `STA_CONNECT_TIMEOUT_MS` (15 s), `setup()` falls back to `NET_SETUP_AP`.

Both modes serve the same `INDEX_HTML`; the JS shows/hides the **Network** card sections based on `s.netMode` from `/api/status`. mDNS (`wemosrfid.local`) and NetBIOS (`\\WEMOSRFID`) are advertised in both modes.

## Build & flash

No CLI build system. Use Arduino IDE 2.x:

- Board: **LOLIN(WEMOS) D1 R2 & mini** (install via ESP8266 boards manager: `https://arduino.esp8266.com/stable/package_esp8266com_index.json`).
- Flash size: pick a layout **with FS** (e.g. `4MB (FS:1MB OTA:~1019KB)`) — LittleFS holds the saved-UID library.
- Library: **MFRC522** by *GithubCommunity* (miguelbalboa fork). `ESP8266WiFi`, `ESP8266WebServer`, `ESP8266mDNS`, `LittleFS`, `SPI` come with the ESP8266 core.
- No tests, no linter — verification is hardware-in-the-loop. Use the Serial monitor at 115200 baud; `PCD_DumpVersionToSerial` on boot is the wiring smoke test (`0x00`/`0xFF` means SPI is broken).

## Architecture

The whole sketch is one `.ino` file built around an explicit state machine and a polled main loop. Reading multiple parts together is the only way to follow a request end-to-end.

**Async command pattern.** HTTP handlers never touch the RC522. Instead, `handleCmd()` validates input, stashes parameters into the global `OpState op`, sets `op.mode`, and returns immediately. The main loop calls `processCard()` on every iteration; when a card is detected it dispatches on `op.mode` and performs the actual SPI work (read/dump/clone/write-uid/write-block), then resets to `MODE_IDLE`. The browser polls `/api/status` (~700 ms) to observe `op.lastResult`, `op.lastUidHex`, etc.

This means a "command" is a two-step interaction: the POST queues intent, the next card tap executes it. Anything that needs to talk to the reader must go through the `OpMode` enum and `processCard()` switch — don't add SPI calls inside HTTP handlers.

**Wi-Fi handlers are the exception** — they bypass the OpState machine because they don't touch the RC522:
- `handleWifiScan` (`GET /api/wifi-scan`) calls `WiFi.scanNetworks()` synchronously (blocks 2–3 s).
- `handleWifiSave` (`POST /api/wifi-save`) accepts a regular form-encoded post (`server.arg("ssid"/"pass")`) — chosen so setup works even with broken JS — writes `/wifi.txt`, then `ESP.restart()`.
- The `wifi-forget` command in `handleCmd` clears `/wifi.txt` and restarts.

**Embedded UI.** The single-page app lives in `INDEX_HTML[] PROGMEM` inside the `.ino`. Editing HTML/CSS/JS = editing C++. The UI is served by `handleRoot()` via `server.send_P`, and only `/`, `/api/status`, `/api/cmd` are routed.

**Hand-rolled JSON.** There is no ArduinoJson dependency. Request parsing uses a `field` lambda inside `handleCmd()` that does naive `indexOf` substring extraction — it assumes flat objects with simple string/number values and will break on nested objects, escaped quotes, or whitespace variations. Responses are built by `String` concatenation; `escJson()` only handles `" \ \n \r \t`. Keep new fields flat and ASCII.

**Persistent saved UIDs.** `loadSaved()` / `writeSaved()` round-trip the entire `/saved.json` file as a raw JSON array string. `forget` mutates by character-index splicing rather than parsing — the array is treated as text. Don't introduce a real JSON parser without rewriting these together.

**LittleFS layout:**
- `/saved.json` — JSON array of UID strings (manipulated as raw text, see above).
- `/wifi.txt` — two-line plain text: SSID, then password. Avoids JSON parsing for credentials and tolerates `"`/`\` in passwords. Created by `saveWifiCreds`, deleted by `clearWifiCreds`.

## Hardware-tied constants

`RST_PIN = 0` (D3 / GPIO0) is also the **ESP8266 boot-mode strap pin**. The RC522 idles RST high, so this works, but if you ever pull it low at reset the chip won't boot. If wiring changes cause boot loops, move `RST_PIN` to `2` (D4 / GPIO2) — this is documented at [README.md:76](README.md) and is a real failure mode, not a hypothetical.

`SS_PIN = 15` (D8 / GPIO15) — SPI CS, fixed by ESP8266 hardware SPI.

## Known quirks

- **AP password is `rfidwemos`** (not `rfidtools` as some older docs claim). Set in [WemosRFID.ino](WemosRFID.ino) as `AP_PASS`.
- **STA scan requires AP_STA mode.** The setup hotspot uses `WIFI_AP_STA` rather than pure `WIFI_AP` so `WiFi.scanNetworks()` works during provisioning. Don't switch it back to `WIFI_AP` without moving the scan elsewhere.
- **Bricking yourself out** — entering wrong creds is recoverable (15 s STA timeout → falls back to setup AP), but a *valid SSID with a wrong password* can sometimes cause `WL_CONNECT_FAILED` quickly enough that you don't get locked out. The "Forget Wi-Fi" button (`wifi-forget` cmd) is the in-band escape hatch; if the device is unreachable, reflash `/wifi.txt` via Arduino IDE's LittleFS uploader or wipe the FS and reformat.
- **UID write only works on "magic" Gen1A** MIFARE Classic cards. `MFRC522::MIFARE_SetUid` will fail silently-ish on genuine cards — surfaced as `"Write failed. Card likely not a magic Gen1A card."`. Also, `MODE_WRITE_UID` / `MODE_CLONE_WRITE_DST` reject 7-byte UIDs (`pendingUidLen != 4`) even though `write-uid` validation accepts 4 or 7 — the magic-card protocol used here is 4-byte only.
- **Dump uses default key A `FF FF FF FF FF FF`** for every sector. Sectors with custom keys print `auth failed` and are skipped. Custom-key support is not implemented.
- **Block-write guardrails** in `handleCmd()` reject blocks `<1`, `>62`, or `% 4 == 3` (sector trailers). Block 0 is also blocked because writing it on non-magic cards bricks auth.
- **Android does not resolve mDNS** — `wemosrfid.local` won't work; use `192.168.4.1`.
