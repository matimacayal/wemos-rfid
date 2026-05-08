# WemosRFID

A web-based RFID toolkit for the **WeMos D1 Mini (ESP-12F / ESP8266)** paired
with an **MFRC522 (RC522) 13.56 MHz** reader/writer module. Serves a single-page
web UI from flash — connect with any phone or laptop and operate the RFID
module from your browser. No app, no cloud, no internet connection required.

The device can either host its own Wi-Fi hotspot **or** join your existing
network — pick whichever is more convenient. On first boot it comes up as a
hotspot for setup; once you save Wi-Fi credentials it joins that network on
every subsequent boot, and you can reach it by name (`wemosrfid.local` /
`\\WEMOSRFID`) without needing to know its IP.

You can save up to **4 networks** (e.g. home + office). On boot the device
scans for visible APs, only tries the saved networks that are actually in
range, and connects to the one with the strongest signal first.

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
| Wi-Fi setup portal            | Pick an SSID from a scan and save creds — no reflash needed     |
| Multi-network Wi-Fi           | Save up to 4 networks; on boot, scans and joins the strongest in-range one |
| Auto-join saved Wi-Fi         | Joins your network on boot; falls back to setup AP if none reachable |
| mDNS + NetBIOS                | Reachable as `http://wemosrfid.local` or `\\WEMOSRFID`          |

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

### First-time setup (Wi-Fi provisioning)

1. Power the WeMos. From a phone or laptop, join the device's hotspot:
   - **SSID:** `WemosRFID`
   - **Password:** `rfidwemos`
2. Open `http://192.168.4.1` (or `http://wemosrfid.local`).
3. In the **Network** card, click **Scan** to populate nearby SSIDs (or type
   one), enter the password, and click **Save & reboot**.
4. The device reboots and joins your Wi-Fi. The `WemosRFID` hotspot disappears.

### After setup

Reach the device on your normal network at one of:

- `http://wemosrfid.local` — macOS, iOS, Linux, Windows 10+
- `\\WEMOSRFID` — Windows file explorer (NetBIOS, for older Windows)
- The IP address shown in the **Network** card (also printed to the serial
  monitor at 115200 baud on every connect, and visible in your router's DHCP
  leases)

Want a stable IP? Set a DHCP reservation in your router using the device's
MAC.

### Adding more Wi-Fi networks (e.g. home + office)

You can save up to **4 networks**. Each new entry is added via the same
**Add a network** form in the **Network** card. The behaviour differs by mode:

- **Adding while connected (station mode)** — saves the new credentials but
  *does not* reboot. The current connection stays up. The new network will be
  considered alongside the others at the next boot.
- **Adding while in setup AP mode** — saves and reboots so the device can try
  to join right away.

On boot the device runs a quick scan, picks only the saved networks that are
currently in range, sorts them by signal strength (highest RSSI first), and
tries them in order. Networks not in range are skipped, so worst-case boot
delay scales with how many *visible* matches you have, not how many you've
saved.

Click the **✕** next to any saved network to remove it (no reboot needed,
even if you're currently connected to that one — the connection stays up
until you reboot or move out of range). **Forget all networks** clears
everything and reboots back into the setup hotspot.

### Day-to-day

1. Tap a card on the RC522 — its UID and type appear at the top.
2. Use the panels to:
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

- `GET  /api/status`    — JSON with presence, last UID, card type, status
  message, IP, `netMode` (`setup` / `station`), `staSSID`, `staIP`,
  `savedWifi` (array of saved SSIDs, in storage order), `savedWifiMax`
  (capacity, currently 4), and the saved-UID array.
- `POST /api/cmd`       — JSON body `{ "cmd": "<name>", ... }`. Commands:
  `read`, `dump`, `save`, `forget` (`index`), `clone-start`, `cancel`,
  `write-uid` (`uid`), `write-block` (`block`, `data`),
  `wifi-remove` (`index` — removes one saved network, no reboot),
  `wifi-forget` (clears all saved networks and reboots).
- `GET  /api/wifi-scan`  — JSON array of nearby networks, sorted by RSSI:
  `[{ "ssid": "...", "rssi": -54, "enc": true }, ...]`.
- `GET  /api/wifi-saved` — JSON array of saved credentials with passwords in
  plaintext: `[{ "ssid": "...", "pass": "..." }, ...]`. Empty `[]` when none
  are persisted. Anyone who can reach the device's HTTP endpoint can read
  these — there is no auth. Index in this array matches the `index` accepted
  by `wifi-remove`.
- `POST /api/wifi-save`  — **form-encoded** (not JSON): `ssid=...&pass=...`.
  Adds the network or replaces the password if the SSID is already saved.
  Returns 400 if the saved-network limit (`savedWifiMax`) is reached. Reboots
  into station mode if currently in setup AP mode; otherwise saves without
  rebooting (the new network is considered at the next boot).

Examples:

```bash
# Status (replace host with wemosrfid.local once on the LAN, or 192.168.4.1 in setup mode)
curl -s http://wemosrfid.local/api/status

# Queue a UID write
curl -s -X POST http://wemosrfid.local/api/cmd \
  -H 'Content-Type: application/json' \
  -d '{"cmd":"write-uid","uid":"DEADBEEF"}'

# Save Wi-Fi credentials (form-encoded). Adds the network if new, or
# replaces its password if the SSID is already saved.
curl -s -X POST http://192.168.4.1/api/wifi-save \
  --data-urlencode 'ssid=MyHomeWifi' \
  --data-urlencode 'pass=hunter2'

# Remove a saved network by index (matches order from /api/wifi-saved)
curl -s -X POST http://wemosrfid.local/api/cmd \
  -H 'Content-Type: application/json' \
  -d '{"cmd":"wifi-remove","index":1}'
```

## Configuration

Edit the constants near the top of `WemosRFID.ino`:

```cpp
constexpr uint8_t  RST_PIN   = 0;           // D3
constexpr uint8_t  SS_PIN    = 15;          // D8
const char*  AP_SSID         = "WemosRFID"; // setup-hotspot SSID
const char*  AP_PASS         = "rfidwemos"; // 8+ chars, WPA2
const char*  MDNS_HOST       = "wemosrfid"; // mDNS + NetBIOS name
constexpr uint32_t STA_CONNECT_TIMEOUT_MS = 15000; // per-network STA join timeout
constexpr uint8_t  MAX_WIFI_CREDS = 4;             // saved-network capacity
```

Saved Wi-Fi credentials live on LittleFS at `/wifi.txt` as paired plain-text
lines: `ssid1\npass1\nssid2\npass2\n...` (up to `MAX_WIFI_CREDS` entries).
Delete the file (or click **Forget all networks** in the UI) to force the
device back into setup mode.

## Troubleshooting

- **`PCD_DumpVersionToSerial` reports `0x00` or `0xFF`** — wiring is wrong or
  the RC522 isn't powered. Double-check 3V3 and the four SPI pins.
- **Boot loops on power-up** — usually a stuck-low GPIO0/D3. Move `RST_PIN`
  to D4 (GPIO2) and re-flash, or temporarily disconnect the RC522 RST wire.
- **`Write failed. Card likely not a magic Gen1A card.`** — the target card
  has a hardcoded UID. Buy "MIFARE 1K UID changeable" / "Magic Gen1A" cards.
- **Dump prints `auth failed`** — the sector isn't using the default key
  `FF FF FF FF FF FF`. Custom-key support isn't built in yet.
- **Can't reach `wemosrfid.local` from Android** — Android doesn't resolve
  mDNS by default. In setup mode, use `192.168.4.1`. In station mode, use
  the IP shown in your router's DHCP leases (or set a DHCP reservation so
  it stays the same), or try `\\WEMOSRFID` on a nearby Windows machine.
- **Saved Wi-Fi creds are wrong / network gone** — at boot the device scans
  first and only attempts saved networks that are visible. If none are in
  range, it falls back to the `WemosRFID` setup hotspot immediately. If a
  network *is* visible but the password is wrong, it tries it for up to 15 s
  before moving on to the next candidate. Reconnect and re-provision.
- **Locked out somehow** — reflash with the Arduino IDE; LittleFS survives
  re-uploads of the sketch unless you also flash a new filesystem image.
  To wipe everything, use *Tools → Erase Flash → All Flash Contents* once.

## Legal / ethical use

This tool is intended for **your own** cards, lab work, learning, and
authorized security testing. Cloning or writing UIDs onto cards used for
access control, transit, or payments without permission is almost certainly
illegal where you live. Don't.

## License

MIT — see source headers.
