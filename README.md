# WemosRFID

A web-based RFID toolkit for the **WeMos D1 Mini (ESP-12F / ESP8266)** paired
with an **MFRC522 (RC522) 13.56 MHz** reader/writer module. The ESP8266 hosts
its own Wi-Fi access point and serves a single-page web UI from flash —
connect with any phone or laptop and operate the RFID module from your
browser. No app, no cloud, no internet connection required.

## Features

| Feature                       | Notes                                                           |
| ----------------------------- | --------------------------------------------------------------- |
| Read UID + card type          | MIFARE Classic / Ultralight / NTAG / DESFire detection          |
| Clone UID                     | Reads source card, then writes its UID to a magic target card   |
| Write a specific UID          | Type a UID (4 or 7 bytes hex) and tap the magic card            |
| Full memory dump              | MIFARE Classic 1K, default key A `FF FF FF FF FF FF`            |
| Write data block              | Any non-trailer block, default keys                             |
| Saved-UID library             | Persisted in LittleFS, browse / load / delete from the UI       |
| Captive web UI                | Embedded HTML/CSS/JS in PROGMEM, no SD card or filesystem upload required |
| mDNS                          | Reachable as `http://wemosrfid.local`                           |

> **Note:** UID writing only works on so-called **"magic" Gen1A** MIFARE
> Classic cards (UID-changeable). Genuine MIFARE cards have a permanent UID
> burned at the factory and cannot be overwritten. The RC522 is also a
> PCD-only chip — it cannot impersonate / emulate a card. For card emulation
> you need a PN532.

## Hardware

- WeMos D1 Mini (ESP-12F based ESP8266)
- MFRC522 RC522 13.56 MHz module
- 6 jumper wires
- USB-C / Micro-USB power for the WeMos

> **Power:** the RC522 runs on **3.3 V** — never connect it to 5 V or you'll
> destroy it. The WeMos 3V3 pin can supply enough current for the RC522.

## Wiring

```
                                  ┌─────────────────────────┐
                                  │      WeMos D1 Mini      │
                                  │       (ESP-12F)         │
                                  │                         │
                                  │  TX  RX  D0  D5  D6  D7 │
                                  │  ┌┐  ┌┐  ┌┐  ┌┐  ┌┐  ┌┐ │
                                  │  └┘  └┘  └┘  └┘  └┘  └┘ │
                                  │                         │
       ┌──────────────┐           │  ┌┐  ┌┐  ┌┐  ┌┐  ┌┐  ┌┐ │
       │   RC522      │           │  └┘  └┘  └┘  └┘  └┘  └┘ │
       │              │           │  RST 3V3 5V GND  D8  D4 │
       │ SDA ─────────┼───────────┼─ D8                     │
       │ SCK ─────────┼───────────┼─ D5                     │
       │ MOSI ────────┼───────────┼─ D7                     │
       │ MISO ────────┼───────────┼─ D6                     │
       │ IRQ          │  (unused) │                         │
       │ GND ─────────┼───────────┼─ GND                    │
       │ RST ─────────┼───────────┼─ D3                     │
       │ 3.3V ────────┼───────────┼─ 3V3                    │
       └──────────────┘           └─────────────────────────┘
```

### Pin map

| RC522 pin | WeMos label | ESP8266 GPIO | Role            |
| --------- | ----------- | ------------ | --------------- |
| SDA / SS  | **D8**      | GPIO15       | SPI chip select |
| SCK       | **D5**      | GPIO14       | SPI clock       |
| MOSI      | **D7**      | GPIO13       | SPI MOSI        |
| MISO      | **D6**      | GPIO12       | SPI MISO        |
| IRQ       | —           | —            | not connected   |
| GND       | **GND**     | —            | ground          |
| RST       | **D3**      | GPIO0        | reader reset    |
| 3.3 V     | **3V3**     | —            | power (3.3 V)   |

> GPIO0 (D3) is also the boot-mode strap pin. It must not be pulled LOW at
> reset, so don't tie the RC522 RST line to ground externally. The RC522
> idles RST high during boot, so this combination works reliably; if you ever
> see boot loops, move RST to D4 (GPIO2) and update `RST_PIN` in the sketch.

## Build & flash

1. Install **Arduino IDE 2.x**.
2. Add the ESP8266 board package: *Preferences → Additional Boards Manager
   URLs* → `https://arduino.esp8266.com/stable/package_esp8266com_index.json`,
   then *Boards Manager → "esp8266" → Install*.
3. Select **Tools → Board → "LOLIN(WEMOS) D1 R2 & mini"**.
4. Set **Tools → Flash Size** to one with FS, e.g. `4MB (FS:1MB OTA:~1019KB)`,
   so LittleFS has space for the saved-UID library.
5. Install the **MFRC522** library (Library Manager, by *GithubCommunity* /
   miguelbalboa).
6. Open `WemosRFID.ino`, select the WeMos serial port, and click **Upload**.

## Using it

1. Power the WeMos. From a phone or laptop, join the Wi-Fi network:
   - **SSID:** `WemosRFID`
   - **Password:** `rfidtools`
2. Open `http://192.168.4.1` (or `http://wemosrfid.local` on most platforms).
3. Tap a card on the RC522 — its UID and type appear at the top.
4. Use the panels to:
   - **Read** the current UID or dump full memory
   - **Clone** a card: press *Start Clone* → tap source → tap magic target
   - **Write UID**: enter `DE AD BE EF` style hex → press the button → tap a
     magic card
   - **Write Block**: choose any block 1–62 (avoid sector trailers 7, 11, 15…)
     and 16 hex bytes
   - **Save** any read UID into the persistent library

The current operation and last result are shown in the *Current Card* panel.
Press *Cancel* to abort a pending operation.

## API (for scripting)

The web UI talks to two simple endpoints:

- `GET  /api/status` — JSON with presence, last UID, card type, status
  message, AP IP, saved-UID array.
- `POST /api/cmd`    — JSON body, `{ "cmd": "<name>", ... }`. Commands:
  `read`, `dump`, `save`, `forget` (`index`), `clone-start`, `cancel`,
  `write-uid` (`uid`), `write-block` (`block`, `data`).

Example:

```bash
curl -s http://192.168.4.1/api/status
curl -s -X POST http://192.168.4.1/api/cmd \
  -H 'Content-Type: application/json' \
  -d '{"cmd":"write-uid","uid":"DEADBEEF"}'
```

## Configuration

Edit the constants near the top of `WemosRFID.ino`:

```cpp
constexpr uint8_t RST_PIN = 0;            // D3
constexpr uint8_t SS_PIN  = 15;           // D8
const char* AP_SSID       = "WemosRFID";
const char* AP_PASS       = "rfidtools";  // 8+ chars, WPA2
const char* MDNS_HOST     = "wemosrfid";
```

## Troubleshooting

- **`PCD_DumpVersionToSerial` reports `0x00` or `0xFF`** — wiring is wrong or
  the RC522 isn't powered. Double-check 3V3 and the four SPI pins.
- **Boot loops on power-up** — usually a stuck-low GPIO0/D3. Move `RST_PIN`
  to D4 (GPIO2) and re-flash, or temporarily disconnect the RC522 RST wire.
- **`Write failed. Card likely not a magic Gen1A card.`** — the target card
  has a hardcoded UID. Buy "MIFARE 1K UID changeable" / "Magic Gen1A" cards.
- **Dump prints `auth failed`** — the sector isn't using the default key
  `FF FF FF FF FF FF`. Custom-key support isn't built in yet.
- **Can't reach `wemosrfid.local`** — Android doesn't resolve mDNS by
  default; use the IP `192.168.4.1` instead.

## Legal / ethical use

This tool is intended for **your own** cards, lab work, learning, and
authorized security testing. Cloning or writing UIDs onto cards used for
access control, transit, or payments without permission is almost certainly
illegal where you live. Don't.

## License

MIT — see source headers.
