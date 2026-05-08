# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

Single-file Arduino sketch (`WemosRFID.ino`) for a **WeMos D1 Mini (ESP8266)** + **MFRC522 (RC522)** RFID reader. The board serves a self-contained web UI for reading/cloning/writing 13.56 MHz cards. There is no separate frontend build, no SD card, no filesystem upload — the UI is a PROGMEM string literal compiled into the firmware.

The device runs in one of two network modes (`NetMode` enum):
- `NET_SETUP_AP` — first boot, no creds, or no saved network was reachable. Hosts the `WemosRFID` hotspot in `WIFI_AP_STA` (STA leg kept alive only so `WiFi.scanNetworks()` works during provisioning).
- `NET_STATION` — joined one of the saved external Wi-Fis from `/wifi.txt` (paired lines: ssid1, pass1, ssid2, pass2, ... up to `MAX_WIFI_CREDS = 4`). On boot, `tryConnectAny()` scans for visible APs, intersects with saved networks, sorts by RSSI desc, and tries each in turn — `STA_CONNECT_TIMEOUT_MS` (15 s) is the **per-network** timeout. Networks not visible in the scan are skipped, so worst-case boot time scales with `(visible-saved-count × 15 s)`, not `(saved-count × 15 s)`.

Both modes serve the same `INDEX_HTML`; the JS renders the **Network** card based on `s.netMode`, `s.savedWifi` (array of SSIDs), and `s.staSSID` from `/api/status`. mDNS (`wemosrfid.local`) and NetBIOS (`\\WEMOSRFID`) are advertised in both modes.

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
- `handleWifiSave` (`POST /api/wifi-save`) accepts form-encoded `ssid`/`pass` (chosen so setup works even with broken JS), then calls `upsertWifiCred()` (matches by SSID — appends or replaces password). Reboot behavior depends on mode: in setup AP mode it `ESP.restart()`s so the device tries to join the new network; in station mode it does NOT reboot — the saved entry is just persisted and tried on next boot, alongside any existing saved networks.
- The `wifi-remove` command in `handleCmd` removes one entry by index (matches the order returned by `/api/wifi-saved` and `/api/status` `savedWifi`); does **not** reboot — the user can remove a saved network without dropping the current connection.
- The `wifi-forget` command in `handleCmd` clears the entire file (`clearWifiCreds()`) and restarts.

**Embedded UI.** The single-page app lives in `INDEX_HTML[] PROGMEM` inside the `.ino`. Editing HTML/CSS/JS = editing C++. The UI is served by `handleRoot()` via `server.send_P`, and only `/`, `/api/status`, `/api/cmd` are routed.

**Hand-rolled JSON.** There is no ArduinoJson dependency. Request parsing uses a `field` lambda inside `handleCmd()` that does naive `indexOf` substring extraction — it assumes flat objects with simple string/number values and will break on nested objects, escaped quotes, or whitespace variations. Responses are built by `String` concatenation; `escJson()` only handles `" \ \n \r \t`. Keep new fields flat and ASCII.

**Persistent saved UIDs.** `loadSaved()` / `writeSaved()` round-trip the entire `/saved.json` file as a raw JSON array string. `forget` mutates by character-index splicing rather than parsing — the array is treated as text. Don't introduce a real JSON parser without rewriting these together.

**LittleFS layout:**
- `/saved.json` — JSON array of UID strings (manipulated as raw text, see above).
- `/wifi.txt` — paired-line plain text: `ssid1\npass1\nssid2\npass2\n...` (up to `MAX_WIFI_CREDS = 4` entries). Avoids JSON parsing for credentials and tolerates `"`/`\` in passwords. Round-tripped by `loadAllWifiCreds` / `saveAllWifiCreds`; mutated by `upsertWifiCred` (match by SSID) and `removeWifiCredAt` (match by index); cleared by `clearWifiCreds`. **Caveat:** passwords can't contain raw `\n`, since the file is line-delimited (in practice WPA2 passphrases don't).

## Hardware-tied constants

`RST_PIN = 0` (D3 / GPIO0) is also the **ESP8266 boot-mode strap pin**. The RC522 idles RST high, so this works, but if you ever pull it low at reset the chip won't boot. If wiring changes cause boot loops, move `RST_PIN` to `2` (D4 / GPIO2) — this is documented at [README.md:76](README.md) and is a real failure mode, not a hypothetical.

`SS_PIN = 15` (D8 / GPIO15) — SPI CS, fixed by ESP8266 hardware SPI.

## Known quirks

- **AP password is `rfidwemos`** (not `rfidtools` as some older docs claim). Set in [WemosRFID.ino](WemosRFID.ino) as `AP_PASS`.
- **STA scan requires AP_STA mode.** The setup hotspot uses `WIFI_AP_STA` rather than pure `WIFI_AP` so `WiFi.scanNetworks()` works during provisioning. Don't switch it back to `WIFI_AP` without moving the scan elsewhere.
- **Bricking yourself out** — entering wrong creds is recoverable. Each saved network is tried in turn with a 15 s timeout, so if you have e.g. home + office saved and only home is in range, only home is attempted. Even if all in-range candidates fail, the device falls back to the setup AP. The "Forget all networks" button (`wifi-forget` cmd) is the in-band nuclear option; "✕" next to a saved network triggers `wifi-remove` (does not reboot). If the device is unreachable, reflash `/wifi.txt` via Arduino IDE's LittleFS uploader or wipe the FS and reformat.
- **Multi-network connect order is RSSI-descending, not save-order.** The strongest in-range signal wins. Two saved networks at the same site means the device connects to whichever has better signal at boot — usually fine, but if you've moved between sites you may end up on a different network than expected.
- **UID write only works on "magic" Gen1A** MIFARE Classic cards. `MFRC522::MIFARE_SetUid` will fail silently-ish on genuine cards — surfaced as `"Write failed. Card likely not a magic Gen1A card."`. Also, `MODE_WRITE_UID` / `MODE_CLONE_WRITE_DST` reject 7-byte UIDs (`pendingUidLen != 4`) even though `write-uid` validation accepts 4 or 7 — the magic-card protocol used here is 4-byte only.
- **Dump uses default key A `FF FF FF FF FF FF`** for every sector. Sectors with custom keys print `auth failed` and are skipped. Custom-key support is not implemented.
- **Block-write guardrails** in `handleCmd()` reject blocks `<1`, `>62`, or `% 4 == 3` (sector trailers). Block 0 is also blocked because writing it on non-magic cards bricks auth.
- **Android does not resolve mDNS** — `wemosrfid.local` won't work; use `192.168.4.1`.
