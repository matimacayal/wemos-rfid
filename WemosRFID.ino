/*
 * WemosRFID — ESP8266 + RC522 web-based RFID toolkit
 * -------------------------------------------------
 * Hardware: WeMos D1 Mini (ESP-12F) + MFRC522 (RC522) module
 *
 * Wiring (RC522 -> WeMos D1 Mini):
 *   SDA / SS  -> D8  (GPIO15)
 *   SCK       -> D5  (GPIO14)
 *   MOSI      -> D7  (GPIO13)
 *   MISO      -> D6  (GPIO12)
 *   RST       -> D3  (GPIO0)
 *   3.3V      -> 3V3
 *   GND       -> GND
 *
 * Required libraries (Arduino IDE -> Library Manager):/Users/matimacayal/Documents/Arduino/WemosRFID/WemosRFID.ino
 *   - "MFRC522" by GithubCommunity (miguelbalboa fork)
 *   - ESP8266WiFi, ESP8266WebServer, ESP8266mDNS, LittleFS (bundled w/ esp8266 core)
 *
 * Board: "LOLIN(WEMOS) D1 R2 & mini" in the ESP8266 boards manager.
 *
 * First-boot / setup: the device hosts a Wi-Fi hotspot
 *   SSID "WemosRFID", password "rfidwemos"
 * Connect to it and open http://192.168.4.1 — the Network card lets you
 * join your home Wi-Fi. Credentials are stored on LittleFS (/wifi.txt) and
 * the device joins that network automatically on subsequent boots.
 *
 * Discovery on the joined network:
 *   - http://wemosrfid.local       (mDNS — macOS, iOS, Linux, Windows 10+)
 *   - \\WEMOSRFID                  (NetBIOS — older Windows / file explorer)
 *   - router DHCP leases           (Android, fallback)
 *   - Serial monitor at 115200     (always prints the IP on connect)
 *
 * If saved credentials fail (network gone, password changed), the device
 * falls back to the WemosRFID hotspot after ~15 s so you can re-provision.
 */

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <ESP8266NetBIOS.h>
#include <LittleFS.h>
#include <SPI.h>
#include <MFRC522.h>

// ---- Configuration ---------------------------------------------------------
constexpr uint8_t  RST_PIN   = 0;          // D3
constexpr uint8_t  SS_PIN    = 15;         // D8
const char*  AP_SSID         = "WemosRFID";
const char*  AP_PASS         = "rfidwemos"; // 8+ chars required by WPA2
const char*  MDNS_HOST       = "wemosrfid";
const char*  SAVED_TXT       = "/saved.txt"; // paired lines: uid1, name1, uid2, name2, ...
const char*  WIFI_FILE       = "/wifi.txt";  // paired lines: ssid1, pass1, ssid2, pass2, ...
constexpr uint32_t STA_CONNECT_TIMEOUT_MS = 15000;  // per-network connect timeout
constexpr uint8_t  MAX_WIFI_CREDS = 4;              // max saved networks
constexpr uint8_t  MAX_SAVED_UIDS = 32;             // max saved UIDs in library

// ---- Globals ---------------------------------------------------------------
MFRC522 mfrc(SS_PIN, RST_PIN);
ESP8266WebServer server(80);

enum NetMode : uint8_t {
  NET_SETUP_AP = 0,   // no creds (or STA failed): hosting WemosRFID hotspot
  NET_STATION         // joined a saved external Wi-Fi
};
NetMode netMode = NET_SETUP_AP;
String  staSSID = "";

enum OpMode : uint8_t {
  MODE_IDLE = 0,
  MODE_CLONE_READ_SRC,
  MODE_CLONE_WRITE_DST,
  MODE_WRITE_UID,
  MODE_DUMP,
  MODE_WRITE_BLOCK
};

struct OpState {
  OpMode   mode = MODE_IDLE;
  uint8_t  pendingUid[10] = {0};
  uint8_t  pendingUidLen  = 0;
  uint8_t  pendingBlock   = 0;
  uint8_t  pendingData[16] = {0};
  String   lastResult     = "Ready.";
  String   lastUidHex     = "";
  String   lastCardType   = "";
  String   lastDump       = "";
  uint32_t lastEventMs    = 0;
} op;

struct WifiCred {
  String ssid;
  String pass;
};
WifiCred wifiCache[MAX_WIFI_CREDS];
uint8_t  wifiCacheN = 0;

struct SavedUid {
  String uid;
  String name;
};
SavedUid savedCache[MAX_SAVED_UIDS];
uint8_t  savedCacheN = 0;

// ---- Status LED ------------------------------------------------------------
// Onboard LED is on GPIO2 / D4 (active LOW: LOW = on, HIGH = off). Exposed as
// LED_BUILTIN by the ESP8266 core.
enum LedMode : uint8_t {
  LED_OFF = 0,     // explicitly off (used briefly during boot)
  LED_CONNECTING,  // 100/100 ms — fast blink while attempting saved networks
  LED_STATION,     // 60/2940 ms — sparse heartbeat once joined
  LED_AP           // 500/500 ms — even 1 Hz blink while hosting setup hotspot
};
LedMode  ledMode       = LED_OFF;
uint32_t ledPhaseStart = 0;
uint8_t  ledPhaseIdx   = 0; // 0 = "on" half of cycle, 1 = "off" half

// ---- Helpers ---------------------------------------------------------------
String bytesToHex(const uint8_t* buf, uint8_t len, bool spaced = false) {
  String s;
  s.reserve(len * (spaced ? 3 : 2));
  for (uint8_t i = 0; i < len; i++) {
    if (buf[i] < 0x10) s += '0';
    s += String(buf[i], HEX);
    if (spaced && i + 1 < len) s += ' ';
  }
  s.toUpperCase();
  return s;
}

bool hexToBytes(const String& hex, uint8_t* out, uint8_t maxLen, uint8_t& outLen) {
  String clean;
  for (size_t i = 0; i < hex.length(); i++) {
    char c = hex[i];
    if (c == ' ' || c == ':' || c == '-') continue;
    clean += c;
  }
  if (clean.length() % 2 != 0) return false;
  outLen = clean.length() / 2;
  if (outLen > maxLen) return false;
  for (uint8_t i = 0; i < outLen; i++) {
    char hi = clean[i * 2];
    char lo = clean[i * 2 + 1];
    auto nyb = [](char c) -> int {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      if (c >= 'A' && c <= 'F') return c - 'A' + 10;
      return -1;
    };
    int h = nyb(hi), l = nyb(lo);
    if (h < 0 || l < 0) return false;
    out[i] = (h << 4) | l;
  }
  return true;
}

String picctypeName(MFRC522::PICC_Type t) {
  switch (t) {
    case MFRC522::PICC_TYPE_ISO_14443_4:    return "ISO/IEC 14443-4";
    case MFRC522::PICC_TYPE_ISO_18092:      return "ISO/IEC 18092 (NFC)";
    case MFRC522::PICC_TYPE_MIFARE_MINI:    return "MIFARE Mini";
    case MFRC522::PICC_TYPE_MIFARE_1K:      return "MIFARE 1K";
    case MFRC522::PICC_TYPE_MIFARE_4K:      return "MIFARE 4K";
    case MFRC522::PICC_TYPE_MIFARE_UL:      return "MIFARE Ultralight / NTAG";
    case MFRC522::PICC_TYPE_MIFARE_PLUS:    return "MIFARE Plus";
    case MFRC522::PICC_TYPE_MIFARE_DESFIRE: return "MIFARE DESFire";
    case MFRC522::PICC_TYPE_TNP3XXX:        return "TNP3XXX";
    case MFRC522::PICC_TYPE_NOT_COMPLETE:   return "SAK incomplete";
    default:                                return "Unknown";
  }
}

bool waitForCard(uint16_t timeoutMs = 0) {
  uint32_t start = millis();
  do {
    if (mfrc.PICC_IsNewCardPresent() && mfrc.PICC_ReadCardSerial()) return true;
    delay(20);
  } while (timeoutMs == 0 ? false : (millis() - start) < timeoutMs);
  return false;
}

// ---- Persistent saved-UID library -----------------------------------------
// File format: paired lines (uid1, name1, uid2, name2, ...). Same rationale as
// /wifi.txt — no JSON parser, tolerant of `"` and `\` in names. Names cannot
// contain `\n` (line-delimited), and the field() request parser cannot handle
// `"` inside names because it does not understand JSON escapes.
bool loadAllSaved(SavedUid* out, uint8_t maxN, uint8_t& outN) {
  outN = 0;
  if (!LittleFS.exists(SAVED_TXT)) return false;
  File f = LittleFS.open(SAVED_TXT, "r");
  if (!f) return false;
  while (f.available() && outN < maxN) {
    String u = f.readStringUntil('\n'); u.trim();
    String n = f.readStringUntil('\n'); n.trim();
    if (u.length() == 0) continue;
    out[outN].uid = u;
    out[outN].name = n;
    outN++;
  }
  f.close();
  Serial.printf("[SAVED] loaded %u saved UID(s)\n", outN);
  return outN > 0;
}

void saveAllSaved(const SavedUid* arr, uint8_t n) {
  if (n == 0) {
    LittleFS.remove(SAVED_TXT);
    Serial.println(F("[SAVED] file removed (empty list)"));
    return;
  }
  File f = LittleFS.open(SAVED_TXT, "w");
  if (!f) {
    Serial.println(F("[SAVED] failed to open file for write"));
    return;
  }
  for (uint8_t i = 0; i < n; i++) {
    f.println(arr[i].uid);
    f.println(arr[i].name);
  }
  f.close();
  Serial.printf("[SAVED] wrote %u saved UID(s)\n", n);
}

void syncSavedCache() {
  loadAllSaved(savedCache, MAX_SAVED_UIDS, savedCacheN);
}

// Returns 1 = added, 0 = already exists (no-op), -1 = list full.
int addSavedUid(const String& uid, const String& name) {
  for (uint8_t i = 0; i < savedCacheN; i++) {
    if (savedCache[i].uid == uid) {
      Serial.printf("[SAVED] UID %s already in library\n", uid.c_str());
      return 0;
    }
  }
  if (savedCacheN >= MAX_SAVED_UIDS) {
    Serial.printf("[SAVED] cannot add %s: list full (%u/%u)\n",
                  uid.c_str(), savedCacheN, MAX_SAVED_UIDS);
    return -1;
  }
  savedCache[savedCacheN].uid  = uid;
  savedCache[savedCacheN].name = name;
  savedCacheN++;
  saveAllSaved(savedCache, savedCacheN);
  Serial.printf("[SAVED] added UID %s (name='%s', now %u/%u)\n",
                uid.c_str(), name.c_str(), savedCacheN, MAX_SAVED_UIDS);
  return 1;
}

bool removeSavedAt(uint8_t idx) {
  if (idx >= savedCacheN) {
    Serial.printf("[SAVED] remove index %u out of range (%u saved)\n",
                  idx, savedCacheN);
    return false;
  }
  String removed = savedCache[idx].uid;
  for (uint8_t i = idx; i + 1 < savedCacheN; i++) savedCache[i] = savedCache[i + 1];
  savedCacheN--;
  savedCache[savedCacheN].uid  = "";
  savedCache[savedCacheN].name = "";
  saveAllSaved(savedCache, savedCacheN);
  Serial.printf("[SAVED] removed UID %s (now %u/%u)\n",
                removed.c_str(), savedCacheN, MAX_SAVED_UIDS);
  return true;
}

bool renameSavedAt(uint8_t idx, const String& name) {
  if (idx >= savedCacheN) {
    Serial.printf("[SAVED] rename index %u out of range (%u saved)\n",
                  idx, savedCacheN);
    return false;
  }
  savedCache[idx].name = name;
  saveAllSaved(savedCache, savedCacheN);
  Serial.printf("[SAVED] renamed [%u] %s -> '%s'\n",
                idx, savedCache[idx].uid.c_str(), name.c_str());
  return true;
}

// One-shot migration from old /saved.json (JSON array of UID strings) to the
// new paired-line /saved.txt. Runs at boot before syncSavedCache(); idempotent
// (skips if /saved.txt already exists).
void migrateSavedJsonIfPresent() {
  const char* OLD = "/saved.json";
  if (!LittleFS.exists(OLD)) return;
  if (LittleFS.exists(SAVED_TXT)) {
    LittleFS.remove(OLD);
    Serial.println(F("[SAVED] /saved.txt already present; removed leftover /saved.json"));
    return;
  }
  File f = LittleFS.open(OLD, "r");
  if (!f) return;
  String s = f.readString();
  f.close();
  Serial.println(F("[SAVED] migrating /saved.json -> /saved.txt"));
  SavedUid tmp[MAX_SAVED_UIDS];
  uint8_t  n = 0;
  int i = 0;
  while (i < (int)s.length() && n < MAX_SAVED_UIDS) {
    int q1 = s.indexOf('"', i);
    if (q1 < 0) break;
    int q2 = s.indexOf('"', q1 + 1);
    if (q2 < 0) break;
    tmp[n].uid  = s.substring(q1 + 1, q2);
    tmp[n].name = "";
    n++;
    i = q2 + 1;
  }
  if (n > 0) saveAllSaved(tmp, n);
  LittleFS.remove(OLD);
  Serial.printf("[SAVED] migrated %u UID(s)\n", n);
}

// ---- Status LED helpers ---------------------------------------------------
void setLedMode(LedMode m) {
  if (m == ledMode) return;
  ledMode       = m;
  ledPhaseStart = millis();
  ledPhaseIdx   = 0;
  if (m == LED_OFF) {
    digitalWrite(LED_BUILTIN, HIGH); // off
  } else {
    digitalWrite(LED_BUILTIN, LOW);  // on (start of "on" half)
  }
}

// Non-blocking. Call frequently from loop() and also from inside the blocking
// connect wait so the LED stays animated during station-join attempts.
void updateLed() {
  if (ledMode == LED_OFF) return;
  uint16_t onMs, offMs;
  switch (ledMode) {
    case LED_CONNECTING: onMs = 100; offMs = 100;  break;
    case LED_STATION:    onMs = 60;  offMs = 2940; break;
    case LED_AP:         onMs = 500; offMs = 500;  break;
    default: return;
  }
  uint32_t now = millis();
  uint16_t dur = (ledPhaseIdx == 0) ? onMs : offMs;
  if (now - ledPhaseStart >= dur) {
    ledPhaseStart = now;
    ledPhaseIdx  ^= 1;
    digitalWrite(LED_BUILTIN, ledPhaseIdx == 0 ? LOW : HIGH);
  }
}

// ---- Wi-Fi credentials store ----------------------------------------------
// Load all saved creds. Returns count via outN. File format is paired lines:
// ssid1\npass1\nssid2\npass2\n... — paired-line is intentional; avoids JSON
// parsing for credentials and tolerates "/\ in passwords (per existing
// design rationale for the single-cred case).
bool loadAllWifiCreds(WifiCred* out, uint8_t maxN, uint8_t& outN) {
  outN = 0;
  if (!LittleFS.exists(WIFI_FILE)) {
    Serial.println(F("[WiFi] no saved creds file"));
    return false;
  }
  File f = LittleFS.open(WIFI_FILE, "r");
  if (!f) {
    Serial.println(F("[WiFi] failed to open creds file"));
    return false;
  }
  while (f.available() && outN < maxN) {
    String s = f.readStringUntil('\n'); s.trim();
    String p = f.readStringUntil('\n'); p.trim();
    if (s.length() == 0) continue;
    out[outN].ssid = s;
    out[outN].pass = p;
    outN++;
  }
  f.close();
  Serial.printf("[WiFi] loaded %u saved network(s)\n", outN);
  for (uint8_t i = 0; i < outN; i++) {
    Serial.printf("[WiFi]   %u) '%s' (pass len=%u)\n",
                  i, out[i].ssid.c_str(), (unsigned)out[i].pass.length());
  }
  return outN > 0;
}

void saveAllWifiCreds(const WifiCred* arr, uint8_t n) {
  if (n == 0) {
    LittleFS.remove(WIFI_FILE);
    Serial.println(F("[WiFi] saved-creds file removed (empty list)"));
    return;
  }
  File f = LittleFS.open(WIFI_FILE, "w");
  if (!f) {
    Serial.println(F("[WiFi] failed to open creds file for write"));
    return;
  }
  for (uint8_t i = 0; i < n; i++) {
    f.println(arr[i].ssid);
    f.println(arr[i].pass);
  }
  f.close();
  Serial.printf("[WiFi] wrote %u saved network(s)\n", n);
}

// Add a new cred or update an existing one (matched by SSID).
// Returns:  1 = added new,  0 = updated existing,  -1 = list full.
int upsertWifiCred(const String& ssid, const String& pass) {
  WifiCred all[MAX_WIFI_CREDS]; uint8_t n = 0;
  loadAllWifiCreds(all, MAX_WIFI_CREDS, n);
  for (uint8_t i = 0; i < n; i++) {
    if (all[i].ssid == ssid) {
      all[i].pass = pass;
      saveAllWifiCreds(all, n);
      syncWifiCache();
      Serial.printf("[WiFi] updated cred for '%s'\n", ssid.c_str());
      return 0;
    }
  }
  if (n >= MAX_WIFI_CREDS) {
    Serial.printf("[WiFi] cannot add '%s': list full (%u/%u)\n",
                  ssid.c_str(), n, MAX_WIFI_CREDS);
    return -1;
  }
  all[n].ssid = ssid;
  all[n].pass = pass;
  n++;
  saveAllWifiCreds(all, n);
  syncWifiCache();
  Serial.printf("[WiFi] added cred for '%s' (now %u/%u)\n",
                ssid.c_str(), n, MAX_WIFI_CREDS);
  return 1;
}

bool removeWifiCredAt(uint8_t idx) {
  WifiCred all[MAX_WIFI_CREDS]; uint8_t n = 0;
  loadAllWifiCreds(all, MAX_WIFI_CREDS, n);
  if (idx >= n) {
    Serial.printf("[WiFi] remove index %u out of range (%u saved)\n", idx, n);
    return false;
  }
  String removed = all[idx].ssid;
  for (uint8_t i = idx; i + 1 < n; i++) all[i] = all[i + 1];
  n--;
  saveAllWifiCreds(all, n);
  syncWifiCache();
  Serial.printf("[WiFi] removed '%s' (now %u/%u)\n",
                removed.c_str(), n, MAX_WIFI_CREDS);
  return true;
}

void clearWifiCreds() {
  LittleFS.remove(WIFI_FILE);
  wifiCacheN = 0;
  Serial.println(F("[WiFi] all credentials cleared"));
}

void syncWifiCache() {
  loadAllWifiCreds(wifiCache, MAX_WIFI_CREDS, wifiCacheN);
}

bool tryConnectSTA(const String& ssid, const String& pass) {
  WiFi.mode(WIFI_STA);
  WiFi.hostname(MDNS_HOST);
  WiFi.begin(ssid.c_str(), pass.c_str());
  Serial.printf("[STA] connecting to '%s' (timeout %lu ms)", ssid.c_str(),
                (unsigned long)STA_CONNECT_TIMEOUT_MS);
  uint32_t start = millis();
  uint32_t lastDot = start;
  while (WiFi.status() != WL_CONNECTED && millis() - start < STA_CONNECT_TIMEOUT_MS) {
    delay(20);
    updateLed();
    if (millis() - lastDot >= 250) { Serial.print('.'); lastDot = millis(); }
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[STA] connected in %lu ms: IP=%s RSSI=%d dBm BSSID=%s ch=%d\n",
                  millis() - start,
                  WiFi.localIP().toString().c_str(),
                  WiFi.RSSI(),
                  WiFi.BSSIDstr().c_str(),
                  WiFi.channel());
    return true;
  }
  Serial.printf("[STA] connect failed after %lu ms (status=%d)\n",
                millis() - start, WiFi.status());
  return false;
}

// Scan-first orchestrator: load all saved creds, scan for visible networks,
// keep only the saved networks that are in range, sort by RSSI desc, and try
// each until one connects. Skips networks that didn't show up in the scan,
// so worst-case boot time is bounded by (visible-saved-count × per-net timeout).
bool tryConnectAny() {
  WifiCred saved[MAX_WIFI_CREDS]; uint8_t nSaved = 0;
  loadAllWifiCreds(saved, MAX_WIFI_CREDS, nSaved);
  if (nSaved == 0) return false;

  WiFi.mode(WIFI_STA);
  WiFi.hostname(MDNS_HOST);
  Serial.printf("[STA] scanning for %u saved network(s)...\n", nSaved);
  uint32_t t0 = millis();
  int n = WiFi.scanNetworks(/*async=*/false, /*show_hidden=*/false);
  Serial.printf("[STA] scan: %d networks visible in %lu ms\n", n, millis() - t0);

  struct Cand { uint8_t savedIdx; int32_t rssi; };
  Cand cands[MAX_WIFI_CREDS];
  uint8_t nCands = 0;
  for (uint8_t s = 0; s < nSaved; s++) {
    int32_t bestRssi = -1000;
    bool found = false;
    for (int i = 0; i < n; i++) {
      if (WiFi.SSID(i) == saved[s].ssid) {
        if (WiFi.RSSI(i) > bestRssi) bestRssi = WiFi.RSSI(i);
        found = true;
      }
    }
    if (found) {
      Serial.printf("[STA] candidate '%s' (%d dBm)\n",
                    saved[s].ssid.c_str(), (int)bestRssi);
      cands[nCands].savedIdx = s;
      cands[nCands].rssi     = bestRssi;
      nCands++;
    } else {
      Serial.printf("[STA] '%s' not in range, skipping\n", saved[s].ssid.c_str());
    }
  }
  WiFi.scanDelete();

  if (nCands == 0) {
    Serial.println(F("[STA] no saved networks in range"));
    return false;
  }

  // Sort candidates by RSSI descending (insertion sort, n <= MAX_WIFI_CREDS).
  for (uint8_t i = 1; i < nCands; i++) {
    Cand key = cands[i];
    int j = (int)i - 1;
    while (j >= 0 && cands[j].rssi < key.rssi) { cands[j + 1] = cands[j]; j--; }
    cands[j + 1] = key;
  }

  for (uint8_t i = 0; i < nCands; i++) {
    const WifiCred& c = saved[cands[i].savedIdx];
    Serial.printf("[STA] attempt %u/%u: '%s' (%d dBm)\n",
                  (unsigned)(i + 1), (unsigned)nCands,
                  c.ssid.c_str(), (int)cands[i].rssi);
    if (tryConnectSTA(c.ssid, c.pass)) {
      staSSID = c.ssid;
      return true;
    }
    WiFi.disconnect(false);
    delay(100);
  }
  Serial.println(F("[STA] all in-range candidates failed"));
  return false;
}

String htmlEscape(const String& in) {
  String o; o.reserve(in.length());
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    switch (c) {
      case '<': o += "&lt;";   break;
      case '>': o += "&gt;";   break;
      case '&': o += "&amp;";  break;
      case '"': o += "&quot;"; break;
      default:  o += c;
    }
  }
  return o;
}

// ---- RFID operations -------------------------------------------------------
String readCardSummary() {
  String uid = bytesToHex(mfrc.uid.uidByte, mfrc.uid.size);
  MFRC522::PICC_Type t = mfrc.PICC_GetType(mfrc.uid.sak);
  op.lastUidHex   = uid;
  op.lastCardType = picctypeName(t);
  op.lastEventMs  = millis();
  return uid;
}

String dumpClassic1K() {
  // Reads sector 0 (always readable on default keys for many cards) and
  // attempts default-key auth for every sector. Returns a multi-line text dump.
  String out;
  MFRC522::MIFARE_Key key;
  for (uint8_t i = 0; i < 6; i++) key.keyByte[i] = 0xFF;

  Serial.printf("[DUMP] start: UID=%s SAK=0x%02X type='%s'\n",
                bytesToHex(mfrc.uid.uidByte, mfrc.uid.size).c_str(),
                mfrc.uid.sak,
                picctypeName(mfrc.PICC_GetType(mfrc.uid.sak)).c_str());
  uint32_t t0 = millis();
  uint8_t okSectors = 0, badSectors = 0;

  out += "UID:  " + bytesToHex(mfrc.uid.uidByte, mfrc.uid.size, true) + "\n";
  out += "SAK:  0x" + String(mfrc.uid.sak, HEX) + "\n";
  out += "Type: " + picctypeName(mfrc.PICC_GetType(mfrc.uid.sak)) + "\n\n";

  for (uint8_t sector = 0; sector < 16; sector++) {
    uint8_t trailer = sector * 4 + 3;
    auto status = mfrc.PCD_Authenticate(MFRC522::PICC_CMD_MF_AUTH_KEY_A, trailer, &key, &mfrc.uid);
    if (status != MFRC522::STATUS_OK) {
      Serial.printf("[DUMP] S%-2u auth failed (default key A)\n", sector);
      out += "S" + String(sector) + ": auth failed (default key A)\n";
      badSectors++;
      continue;
    }
    Serial.printf("[DUMP] S%-2u auth ok\n", sector);
    okSectors++;
    for (uint8_t blk = sector * 4; blk <= trailer; blk++) {
      uint8_t buf[18]; uint8_t sz = sizeof(buf);
      auto rs = mfrc.MIFARE_Read(blk, buf, &sz);
      if (rs == MFRC522::STATUS_OK) {
        String hex = bytesToHex(buf, 16, true);
        Serial.printf("[DUMP]   blk %2u: %s\n", blk, hex.c_str());
        out += "  blk " + String(blk) + ": " + hex + "\n";
      } else {
        Serial.printf("[DUMP]   blk %2u: read error\n", blk);
        out += "  blk " + String(blk) + ": read error\n";
      }
    }
  }
  mfrc.PICC_HaltA();
  mfrc.PCD_StopCrypto1();
  Serial.printf("[DUMP] done in %lu ms (auth ok=%u, fail=%u)\n",
                millis() - t0, okSectors, badSectors);
  return out;
}

bool writeBlockClassic(uint8_t block, const uint8_t* data16) {
  MFRC522::MIFARE_Key key;
  for (uint8_t i = 0; i < 6; i++) key.keyByte[i] = 0xFF;
  uint8_t trailer = (block / 4) * 4 + 3;
  Serial.printf("[WRITE-BLK] block=%u (sector=%u trailer=%u) data=%s\n",
                block, block / 4, trailer,
                bytesToHex(data16, 16, true).c_str());
  auto status = mfrc.PCD_Authenticate(MFRC522::PICC_CMD_MF_AUTH_KEY_A, trailer, &key, &mfrc.uid);
  if (status != MFRC522::STATUS_OK) {
    Serial.printf("[WRITE-BLK] auth failed (status=%d) on trailer %u\n", status, trailer);
    mfrc.PICC_HaltA();
    mfrc.PCD_StopCrypto1();
    return false;
  }
  status = mfrc.MIFARE_Write(block, (uint8_t*)data16, 16);
  mfrc.PICC_HaltA();
  mfrc.PCD_StopCrypto1();
  Serial.printf("[WRITE-BLK] %s (status=%d)\n",
                status == MFRC522::STATUS_OK ? "OK" : "FAILED", status);
  return status == MFRC522::STATUS_OK;
}

// ---- Web UI (single-page, embedded) ---------------------------------------
const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8"/>
<meta name="viewport" content="width=device-width,initial-scale=1"/>
<title>WemosRFID</title>
<style>
:root { --bg:#0e1116; --fg:#e6e6e6; --mut:#9aa0a6; --acc:#5cc8ff; --ok:#67e480; --bad:#ff7676; --pan:#161b22; }
*{box-sizing:border-box} body{margin:0;background:var(--bg);color:var(--fg);font:15px/1.4 -apple-system,system-ui,Segoe UI,Roboto,sans-serif}
header{padding:14px 18px;border-bottom:1px solid #222;display:flex;justify-content:space-between;align-items:center}
header h1{margin:0;font-size:18px;color:var(--acc)}
.dot{width:10px;height:10px;border-radius:50%;background:#444;display:inline-block;margin-right:6px;vertical-align:middle}
.dot.on{background:var(--ok);box-shadow:0 0 6px var(--ok)}
main{padding:16px;max-width:780px;margin:0 auto;display:grid;gap:14px}
.card{background:var(--pan);border:1px solid #222;border-radius:10px;padding:14px}
.card h2{margin:0 0 10px;font-size:14px;color:var(--acc);text-transform:uppercase;letter-spacing:.05em}
.row{display:flex;gap:8px;flex-wrap:wrap}
button{background:#1f2630;color:var(--fg);border:1px solid #2c3340;border-radius:6px;padding:8px 12px;cursor:pointer;font-size:14px}
button:hover{border-color:var(--acc)}
button.primary{background:#163a52;border-color:#1f5d85}
button.danger{background:#3a1d1d;border-color:#7a2c2c}
input[type=text],input[type=password]{background:#0b0f14;color:var(--fg);border:1px solid #2c3340;border-radius:6px;padding:8px;font-family:ui-monospace,Menlo,Consolas,monospace;font-size:14px;flex:1;min-width:160px}
.kv{font-family:ui-monospace,Menlo,Consolas,monospace}
.muted{color:var(--mut);font-size:13px}
pre{background:#0b0f14;border:1px solid #222;border-radius:6px;padding:10px;overflow:auto;max-height:280px;font:12px/1.45 ui-monospace,Menlo,Consolas,monospace}
.uid{font:600 18px ui-monospace,Menlo,Consolas,monospace;color:var(--ok);letter-spacing:.08em}
.tag{display:inline-block;padding:2px 8px;border:1px solid #2c3340;border-radius:99px;font-size:12px;color:var(--mut);margin-left:6px}
ul.saved{list-style:none;margin:0;padding:0}
ul.saved li{display:flex;justify-content:space-between;align-items:center;padding:6px 0;border-bottom:1px dashed #222}
ul.saved li:last-child{border:0}
.flash{padding:8px 12px;border-radius:6px;background:#173a2a;border:1px solid #245d3f;color:#cdebd5}
.flash.err{background:#3a1d1d;border-color:#7a2c2c;color:#f3c8c8}
.warn{background:#3a2f12;border:1px solid #75590f;border-radius:6px;padding:8px 10px;color:#e9d8a3;font-size:13px}
.spacing{padding:8px 12px;border-radius:6px;background:#173a2a;border:1px solid #245d3f;color:#cdebd5}
footer{padding:16px;color:var(--mut);text-align:center;font-size:12px}
</style>
</head>
<body>
<header>
  <h1>WemosRFID</h1>
  <div><span id="dot" class="dot"></span><span id="presence" class="muted">no card</span></div>
</header>
<main>

  <div class="card">
    <h2>Current Card</h2>
    <div>UID: <span id="uid" class="uid">--</span><span id="ctype" class="tag"></span></div>
    <div class="muted" id="status">Ready.</div>
  </div>

  <div id="flash"></div>

  <div class="card">
    <h2>Read</h2>
    <div class="row">
      <button class="primary" onclick="cmd('read')">Read UID</button>
      <button onclick="cmd('dump')">Full Memory Dump (MIFARE 1K)</button>
      <button onclick="saveUid()">Save UID to library</button>
    </div>
    <!--
    <div class="row" style="margin-top:6px">
      <button onclick="saveUid()">Save UID to library</button>
      <input id="saveName" type="text" placeholder="optional name (e.g. home key)" style="flex:1" maxlength="40"/>
    </div>
    <pre id="dump" style="display:none"></pre>
    -->
  </div>

  <div class="card">
    <h2>Clone</h2>
    <div class="muted">Reads the UID from a source card, then writes it to a magic (UID-changeable) target card.</div>
    <div class="row" style="margin-top:8px">
      <button class="primary" onclick="cmd('clone-start')">Start Clone</button>
      <button onclick="cmd('cancel')">Cancel</button>
    </div>
  </div>

  <div class="card">
    <h2>Write UID</h2>
    <div class="row">
      <input id="uidIn" type="text" placeholder="DE AD BE EF" maxlength="29"/>
      <button class="primary" onclick="writeUid()">Write to next card</button>
    </div>
    <div class="warn" style="margin-top:8px">Requires a "magic" Gen1A UID-changeable MIFARE Classic card. Standard cards have a permanent UID.</div>
  </div>

  <div class="card">
    <h2>Write Block (MIFARE 1K, default keys)</h2>
    <div class="row">
      <input id="blkIn" type="text" placeholder="block (4..62, avoid 0/trailers)" style="max-width:220px"/>
      <input id="dataIn" type="text" placeholder="16-byte hex, e.g. 00 11 22 .. (32 hex chars)"/>
    </div>
    <div class="row" style="margin-top:6px">
      <button class="primary" onclick="writeBlock()">Write to next card</button>
    </div>
  </div>

  <div class="card">
    <h2>Saved UIDs</h2>
    <ul id="saved" class="saved"></ul>
    <div class="muted" id="savedEmpty" style="display:none">No saved UIDs yet.</div>
  </div>

  <div id=spacing></div>

  <div class="card" id="wifiCard">
    <h2>Network</h2>
    <div id="wifiBanner" class="muted"></div>

    <div style="margin-top:10px">
      <div class="muted">Saved networks (on boot: scan, then try in-range ones by signal strength):</div>
      <ul id="wifiSavedList" class="saved"></ul>
      <div class="muted" id="wifiSavedEmpty" style="display:none">No saved networks yet — add one below.</div>
    </div>

    <div id="wifiAdd" style="margin-top:12px">
      <div class="muted" style="margin-bottom:6px">Add a network <span id="wifiCount" class="tag"></span></div>
      <form method="POST" action="/api/wifi-save">
        <div class="row">
          <input list="ssidList" name="ssid" placeholder="SSID" autocomplete="off" required style="flex:1"/>
          <button type="button" onclick="scanWifi()">Scan</button>
        </div>
        <datalist id="ssidList"></datalist>
        <div class="row" style="margin-top:6px">
          <input id="passIn" name="pass" type="password" placeholder="password (blank for open networks)" style="flex:1"/>
          <button type="button" id="passToggle" onclick="togglePassVis()" title="Show/hide password" aria-label="Show password">show</button>
          <button class="primary" type="submit" id="wifiSaveBtn">Save</button>
        </div>
      </form>
      <div class="warn" id="wifiAddWarn" style="margin-top:8px"></div>
    </div>

    <div class="row" id="wifiNuke" style="margin-top:10px;display:none">
      <button class="danger" onclick="forgetAllWifi()">Forget all networks (back to setup)</button>
    </div>
  </div>

</main>
<footer>WeMos D1 Mini + RC522 &middot; AP "WemosRFID" &middot; <span id="ip"></span></footer>

<script>
let dumpEl = document.getElementById('dump');
function flash(msg, err){
  const f = document.getElementById('flash');
  f.innerHTML = '<div class="flash'+(err?' err':'')+'">'+msg+'</div>';
  setTimeout(()=>{ if(f.firstChild) f.firstChild.style.opacity=0.5; }, 2500);
}
async function api(path, body){
  const opts = body ? {method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)} : {};
  const r = await fetch(path, opts);
  return r.json();
}
async function cmd(name){
  const r = await api('/api/cmd', {cmd:name});
  flash(r.message || 'OK', !r.ok);
  if(r.dump){ dumpEl.style.display='block'; dumpEl.textContent = r.dump; }
}
async function saveUid(){
  const nameEl = document.getElementById('saveName');
  // Names are stored line-delimited and sent through a naive JSON parser, so
  // strip newlines and double quotes here to keep round-trips clean.
  const name = nameEl.value.replace(/[\r\n"]/g, '').trim();
  const r = await api('/api/cmd', {cmd:'save', name:name});
  flash(r.message || 'OK', !r.ok);
  if (r.ok) nameEl.value = '';
}
async function writeUid(){
  const v = document.getElementById('uidIn').value.trim();
  if(!v){ flash('Enter a UID', true); return; }
  const r = await api('/api/cmd', {cmd:'write-uid', uid:v});
  flash(r.message || 'queued', !r.ok);
}
async function writeBlock(){
  const b = parseInt(document.getElementById('blkIn').value,10);
  const d = document.getElementById('dataIn').value.trim();
  if(isNaN(b) || !d){ flash('Block + 16 hex bytes required', true); return; }
  const r = await api('/api/cmd', {cmd:'write-block', block:b, data:d});
  flash(r.message || 'queued', !r.ok);
}
function togglePassVis(){
  const el  = document.getElementById('passIn');
  const btn = document.getElementById('passToggle');
  const showing = el.type === 'password';
  el.type = showing ? 'text' : 'password';
  btn.textContent = showing ? 'hide' : 'show';
  btn.setAttribute('aria-label', showing ? 'Hide password' : 'Show password');
}
async function scanWifi(){
  flash('Scanning…');
  try {
    const r = await fetch('/api/wifi-scan');
    const list = await r.json();
    const dl = document.getElementById('ssidList');
    dl.innerHTML = '';
    list.sort((a,b)=>b.rssi-a.rssi).forEach(n=>{
      const o = document.createElement('option');
      o.value = n.ssid;
      o.textContent = n.ssid + ' (' + n.rssi + ' dBm' + (n.enc ? '' : ', open') + ')';
      dl.appendChild(o);
    });
    flash('Found ' + list.length + ' networks');
  } catch(e){ flash('Scan failed', true); }
}
var escHtml = function(s){ return String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/"/g,'&quot;').replace(/'/g,'&#39;'); };
var forgetAllWifi = async function(){
  if(!confirm('Forget ALL saved Wi-Fi networks and reboot? The device will come up as the WemosRFID hotspot.')) return;
  await api('/api/cmd', {cmd:'wifi-forget'});
  flash('All forgotten. Device is rebooting — reconnect to the WemosRFID hotspot.');
}
var removeWifi = async function(idx, ssid){
  if(!confirm('Remove "'+ssid+'" from saved networks?')) return;
  const r = await api('/api/cmd', {cmd:'wifi-remove', index:idx});
  flash(r.message || 'Removed', !r.ok);
  refresh();
}
var renderWifiCard = function(s){
  const banner = document.getElementById('wifiBanner');
  const nuke   = document.getElementById('wifiNuke');
  if (s.netMode === 'station') {
    banner.innerHTML = 'Connected to <b class="kv">'+escHtml(s.staSSID||'?')+'</b> &middot; IP <span class="kv">'+escHtml(s.staIP||s.ip||'?')+'</span>';
    nuke.style.display = 'flex';
  } else {
    banner.textContent = 'Hotspot mode — pick a saved network below or add one. Saving the first network reboots the device into station mode.';
    nuke.style.display = 'none';
  }
  const list = document.getElementById('wifiSavedList');
  list.innerHTML = '';
  const saved = s.savedWifi || [];
  saved.forEach((ssid, i) => {
    const li = document.createElement('li');
    const isCurrent = (s.netMode === 'station' && ssid === s.staSSID);
    const left = document.createElement('span');
    left.innerHTML = '<span class="kv">'+escHtml(ssid)+'</span>'+(isCurrent ? ' <span class="tag" style="color:#67e480;border-color:#245d3f">connected</span>' : '');
    const right = document.createElement('span');
    const delBtn = document.createElement('button');
    delBtn.textContent = '✕'; delBtn.className='danger';
    delBtn.title = 'Remove from saved networks';
    delBtn.onclick = ()=> removeWifi(i, ssid);
    right.appendChild(delBtn);
    li.appendChild(left); li.appendChild(right);
    list.appendChild(li);
  });
  document.getElementById('wifiSavedEmpty').style.display = saved.length ? 'none' : 'block';
  const max = s.savedWifiMax || 4;
  document.getElementById('wifiCount').textContent = saved.length + '/' + max;
  const full = saved.length >= max;
  const saveBtn = document.getElementById('wifiSaveBtn');
  const warn = document.getElementById('wifiAddWarn');
  saveBtn.disabled = full;
  if (full) {
    warn.textContent = 'Maximum saved networks reached. Remove one to add another.';
  } else if (s.netMode === 'station') {
    saveBtn.textContent = 'Save';
    warn.textContent = 'Saving keeps the current connection. The new network is tried at the next boot, alongside the others — strongest signal wins.';
  } else {
    saveBtn.textContent = 'Save & reboot';
    warn.textContent = 'Saving reboots the device. On boot it scans, then connects to the strongest in-range saved network. Find it at http://wemosrfid.local once joined; on Android, check your router\'s DHCP leases.';
  }
}
let _scannedOnce = false;
var refresh = async function(){
  try{
    const s = await api('/api/status');
    document.getElementById('dot').className = 'dot' + (s.present ? ' on':'');
    document.getElementById('presence').textContent = s.present ? 'card present' : 'no card';
    document.getElementById('uid').textContent = s.uid || '--';
    document.getElementById('ctype').textContent = s.cardType || '';
    document.getElementById('status').textContent = s.status || '';
    document.getElementById('ip').textContent = s.ip || '';
    const list = document.getElementById('saved');
    list.innerHTML = '';
    (s.saved||[]).forEach((entry,i)=>{
      const uid  = entry.uid || '';
      const name = entry.name || '';
      const li = document.createElement('li');
      const left = document.createElement('span');
      left.innerHTML = (name ? '<b>'+escHtml(name)+'</b> &middot; ' : '') +
                       '<span class="kv">'+escHtml(uid)+'</span>';
      const right = document.createElement('span');
      const useBtn = document.createElement('button');
      useBtn.textContent = 'Load to writer';
      useBtn.onclick = ()=>{ document.getElementById('uidIn').value = uid; };
      const renBtn = document.createElement('button');
      renBtn.textContent = '\u270E';
      renBtn.title = 'Rename';
      renBtn.onclick = async ()=>{
        const newName = prompt('Name for ' + uid + ' (blank to clear):', name);
        if (newName === null) return;
        const cleaned = newName.replace(/[\r\n"]/g, '').trim();
        const r = await api('/api/cmd', {cmd:'rename', index:i, name:cleaned});
        flash(r.message || 'Renamed', !r.ok);
        refresh();
      };
      const delBtn = document.createElement('button');
      delBtn.textContent = '\u2715'; delBtn.className='danger';
      delBtn.onclick = async ()=>{ await api('/api/cmd',{cmd:'forget',index:i}); refresh(); };
      right.appendChild(useBtn);
      right.appendChild(renBtn);
      right.appendChild(delBtn);
      li.appendChild(left);
      li.appendChild(right);
      list.appendChild(li);
    });
    document.getElementById('savedEmpty').style.display = (s.saved && s.saved.length) ? 'none' : 'block';
    renderWifiCard(s);
    if (s.netMode !== 'station' && !_scannedOnce) { _scannedOnce = true; scanWifi(); }
  }catch(e){}
}
setInterval(refresh, 700); refresh();
</script>
</body></html>
)HTML";

// ---- HTTP handlers ---------------------------------------------------------
void sendJson(int code, const String& body) {
  server.send(code, "application/json", body);
}

String escJson(const String& in) {
  String o; o.reserve(in.length() + 8);
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if (c == '\n') o += "\\n";
    else if (c == '\r') o += "\\r";
    else if (c == '\t') o += "\\t";
    else o += c;
  }
  return o;
}

void handleRoot() {
  Serial.printf("[HTTP] GET / from %s\n",
                server.client().remoteIP().toString().c_str());
  server.sendHeader("Connection", "close");
  server.send_P(200, "text/html", INDEX_HTML);
}

void handleStatus() {
  bool present = (op.lastEventMs && (millis() - op.lastEventMs) < 1500);
  String ip = (netMode == NET_STATION) ? WiFi.localIP().toString() : WiFi.softAPIP().toString();

  String savedWifi = "[";
  for (uint8_t i = 0; i < wifiCacheN; i++) {
    if (i) savedWifi += ',';
    savedWifi += "\"" + escJson(wifiCache[i].ssid) + "\"";
  }
  savedWifi += "]";

  String json = "{";
  json += "\"present\":" + String(present ? "true" : "false");
  json += ",\"uid\":\"" + op.lastUidHex + "\"";
  json += ",\"cardType\":\"" + escJson(op.lastCardType) + "\"";
  json += ",\"status\":\"" + escJson(op.lastResult) + "\"";
  json += ",\"mode\":" + String((int)op.mode);
  json += ",\"ip\":\"" + ip + "\"";
  json += ",\"netMode\":\"" + String(netMode == NET_STATION ? "station" : "setup") + "\"";
  json += ",\"staSSID\":\"" + escJson(staSSID) + "\"";
  json += ",\"staIP\":\"" + (netMode == NET_STATION ? WiFi.localIP().toString() : String("")) + "\"";
  json += ",\"savedWifi\":" + savedWifi;
  json += ",\"savedWifiMax\":" + String((int)MAX_WIFI_CREDS);
  String savedJ = "[";
  for (uint8_t i = 0; i < savedCacheN; i++) {
    if (i) savedJ += ',';
    savedJ += "{\"uid\":\"" + escJson(savedCache[i].uid) + "\"";
    savedJ += ",\"name\":\"" + escJson(savedCache[i].name) + "\"}";
  }
  savedJ += "]";
  json += ",\"saved\":" + savedJ;
  json += ",\"savedMax\":" + String((int)MAX_SAVED_UIDS);
  json += "}";
  sendJson(200, json);
}

void handleCmd() {
  if (!server.hasArg("plain")) {
    Serial.println(F("[HTTP] POST /api/cmd: missing body -> 400"));
    sendJson(400, "{\"ok\":false,\"message\":\"missing body\"}");
    return;
  }
  String body = server.arg("plain");
  Serial.printf("[HTTP] POST /api/cmd from %s body=%s\n",
                server.client().remoteIP().toString().c_str(), body.c_str());

  auto field = [&](const char* k) -> String {
    String key = String("\"") + k + "\"";
    int i = body.indexOf(key);
    if (i < 0) return "";
    int colon = body.indexOf(':', i);
    if (colon < 0) return "";
    int j = colon + 1;
    while (j < (int)body.length() && body[j] == ' ') j++;
    if (j >= (int)body.length()) return "";
    if (body[j] == '"') {
      int end = body.indexOf('"', j + 1);
      return end < 0 ? String("") : body.substring(j + 1, end);
    }
    int end = j;
    while (end < (int)body.length() && body[end] != ',' && body[end] != '}') end++;
    return body.substring(j, end);
  };

  String cmd = field("cmd");

  if (cmd == "read") {
    Serial.println(F("[CMD] read: arming for next card"));
    op.mode = MODE_IDLE;
    op.lastResult = "Place a card on the reader.";
    sendJson(200, "{\"ok\":true,\"message\":\"Place a card on the reader.\"}");
    return;
  }
  if (cmd == "dump") {
    Serial.println(F("[CMD] dump: arming for next card"));
    op.mode = MODE_DUMP;
    op.lastResult = "Place a MIFARE Classic 1K card to dump.";
    sendJson(200, "{\"ok\":true,\"message\":\"Place a card to dump.\"}");
    return;
  }
  if (cmd == "save") {
    if (op.lastUidHex.length() == 0) {
      Serial.println(F("[CMD] save: no UID read yet"));
      sendJson(200, "{\"ok\":false,\"message\":\"No UID read yet.\"}"); return;
    }
    String name = field("name");
    int r = addSavedUid(op.lastUidHex, name);
    if (r == 0) { sendJson(200, "{\"ok\":false,\"message\":\"Already saved.\"}"); return; }
    if (r < 0) {
      String msg = "Saved-UID limit reached (" + String((int)MAX_SAVED_UIDS) + ").";
      sendJson(200, "{\"ok\":false,\"message\":\"" + escJson(msg) + "\"}"); return;
    }
    sendJson(200, "{\"ok\":true,\"message\":\"Saved.\"}");
    return;
  }
  if (cmd == "forget") {
    int idx = field("index").toInt();
    Serial.printf("[CMD] forget index=%d\n", idx);
    if (idx < 0 || !removeSavedAt((uint8_t)idx)) {
      sendJson(200, "{\"ok\":false,\"message\":\"Index out of range.\"}"); return;
    }
    sendJson(200, "{\"ok\":true,\"message\":\"Removed.\"}");
    return;
  }
  if (cmd == "rename") {
    int idx = field("index").toInt();
    String name = field("name");
    Serial.printf("[CMD] rename index=%d name='%s'\n", idx, name.c_str());
    if (idx < 0 || !renameSavedAt((uint8_t)idx, name)) {
      sendJson(200, "{\"ok\":false,\"message\":\"Index out of range.\"}"); return;
    }
    sendJson(200, "{\"ok\":true,\"message\":\"Renamed.\"}");
    return;
  }
  if (cmd == "clone-start") {
    Serial.println(F("[CMD] clone-start: waiting for SOURCE card"));
    op.mode = MODE_CLONE_READ_SRC;
    op.lastResult = "Place SOURCE card to read its UID.";
    sendJson(200, "{\"ok\":true,\"message\":\"Place SOURCE card.\"}");
    return;
  }
  if (cmd == "cancel") {
    Serial.printf("[CMD] cancel (was mode=%d)\n", (int)op.mode);
    op.mode = MODE_IDLE;
    op.pendingUidLen = 0;
    op.lastResult = "Cancelled.";
    sendJson(200, "{\"ok\":true,\"message\":\"Cancelled.\"}");
    return;
  }
  if (cmd == "wifi-forget") {
    Serial.println(F("[CMD] wifi-forget: clearing all creds and restarting"));
    clearWifiCreds();
    sendJson(200, "{\"ok\":true,\"message\":\"All Wi-Fi networks forgotten. Rebooting.\"}");
    delay(400);
    ESP.restart();
    return;
  }
  if (cmd == "wifi-remove") {
    int idx = field("index").toInt();
    Serial.printf("[CMD] wifi-remove index=%d\n", idx);
    if (idx < 0 || !removeWifiCredAt((uint8_t)idx)) {
      sendJson(200, "{\"ok\":false,\"message\":\"Index out of range.\"}");
      return;
    }
    sendJson(200, "{\"ok\":true,\"message\":\"Network removed.\"}");
    return;
  }
  if (cmd == "write-uid") {
    String uid = field("uid");
    uint8_t buf[10]; uint8_t len = 0;
    if (!hexToBytes(uid, buf, sizeof(buf), len) || (len != 4 && len != 7)) {
      Serial.printf("[CMD] write-uid rejected: bad UID '%s'\n", uid.c_str());
      sendJson(200, "{\"ok\":false,\"message\":\"UID must be 4 or 7 hex bytes.\"}"); return;
    }
    memcpy(op.pendingUid, buf, len);
    op.pendingUidLen = len;
    op.mode = MODE_WRITE_UID;
    op.lastResult = "Place magic card to write UID " + bytesToHex(buf, len) + ".";
    Serial.printf("[CMD] write-uid: queued UID %s (%u bytes), waiting for magic card\n",
                  bytesToHex(buf, len).c_str(), len);
    sendJson(200, "{\"ok\":true,\"message\":\"Place magic card.\"}");
    return;
  }
  if (cmd == "write-block") {
    int blk = field("block").toInt();
    String data = field("data");
    uint8_t buf[16]; uint8_t len = 0;
    if (!hexToBytes(data, buf, sizeof(buf), len) || len != 16) {
      Serial.printf("[CMD] write-block rejected: bad data '%s' (len=%u)\n",
                    data.c_str(), len);
      sendJson(200, "{\"ok\":false,\"message\":\"Data must be exactly 16 bytes.\"}"); return;
    }
    if (blk < 1 || blk > 62 || blk % 4 == 3) {
      Serial.printf("[CMD] write-block rejected: bad block %d\n", blk);
      sendJson(200, "{\"ok\":false,\"message\":\"Block must be 1..62 and not a sector trailer.\"}"); return;
    }
    op.pendingBlock = blk;
    memcpy(op.pendingData, buf, 16);
    op.mode = MODE_WRITE_BLOCK;
    op.lastResult = "Place card to write block " + String(blk) + ".";
    Serial.printf("[CMD] write-block: queued blk=%d data=%s, waiting for card\n",
                  blk, bytesToHex(buf, 16, true).c_str());
    sendJson(200, "{\"ok\":true,\"message\":\"Place card.\"}");
    return;
  }

  Serial.printf("[CMD] unknown command '%s'\n", cmd.c_str());
  sendJson(200, "{\"ok\":false,\"message\":\"Unknown command\"}");
}

void handleNotFound() {
  Serial.printf("[HTTP] 404 %s %s from %s\n",
                server.method() == HTTP_GET ? "GET" : "POST",
                server.uri().c_str(),
                server.client().remoteIP().toString().c_str());
  server.send(404, "text/plain", "Not found");
}

void handleWifiSaved() {
  // Returns a JSON array of saved networks with passwords in plaintext —
  // consistent with the rest of the unauthenticated API. Index in this array
  // matches the `index` accepted by `wifi-remove`.
  Serial.printf("[HTTP] GET /api/wifi-saved from %s\n",
                server.client().remoteIP().toString().c_str());
  String json = "[";
  for (uint8_t i = 0; i < wifiCacheN; i++) {
    if (i) json += ',';
    json += "{\"ssid\":\"" + escJson(wifiCache[i].ssid) + "\",";
    json += "\"pass\":\"" + escJson(wifiCache[i].pass) + "\"}";
  }
  json += "]";
  sendJson(200, json);
}

void handleWifiScan() {
  // ESP8266 needs the STA radio active for scanNetworks(). In NET_SETUP_AP we
  // already brought the chip up in WIFI_AP_STA so this just works; in station
  // mode we're already STA.
  Serial.printf("[HTTP] GET /api/wifi-scan from %s\n",
                server.client().remoteIP().toString().c_str());
  uint32_t t0 = millis();
  int n = WiFi.scanNetworks(/*async=*/false, /*show_hidden=*/false);
  Serial.printf("[WiFi] scan returned %d networks in %lu ms\n", n, millis() - t0);
  String json = "[";
  for (int i = 0; i < n; i++) {
    if (i) json += ',';
    json += "{\"ssid\":\"" + escJson(WiFi.SSID(i)) + "\"";
    json += ",\"rssi\":" + String(WiFi.RSSI(i));
    json += ",\"enc\":" + String(WiFi.encryptionType(i) != ENC_TYPE_NONE ? "true" : "false");
    json += "}";
    Serial.printf("[WiFi]   %2d) %-32s %4d dBm %s\n", i,
                  WiFi.SSID(i).c_str(), WiFi.RSSI(i),
                  WiFi.encryptionType(i) != ENC_TYPE_NONE ? "secured" : "open");
  }
  json += "]";
  WiFi.scanDelete();
  sendJson(200, json);
}

void handleWifiSave() {
  Serial.printf("[HTTP] POST /api/wifi-save from %s\n",
                server.client().remoteIP().toString().c_str());
  String ssid = server.arg("ssid");
  String pass = server.arg("pass");
  ssid.trim();
  if (ssid.length() == 0) {
    Serial.println(F("[WiFi] save rejected: SSID required"));
    server.send(400, "text/plain", "SSID required");
    return;
  }
  int r = upsertWifiCred(ssid, pass);
  if (r < 0) {
    String msg = "Saved-network limit reached (" + String((int)MAX_WIFI_CREDS) +
                 "). Remove one before adding another.";
    server.send(400, "text/plain", msg);
    return;
  }
  String safeSsid = htmlEscape(ssid);
  bool reboot = (netMode == NET_SETUP_AP);
  Serial.printf("[WiFi] saved '%s' (%s); will %s\n",
                ssid.c_str(), r == 1 ? "added" : "updated",
                reboot ? "reboot to join" : "keep current connection");

  String body;
  if (reboot) {
    body =
      "<!doctype html><html><head><meta charset=utf-8>"
      "<meta name=viewport content='width=device-width,initial-scale=1'>"
      "<title>WemosRFID — Wi-Fi saved</title>"
      "<style>body{margin:0;background:#0e1116;color:#e6e6e6;font:15px/1.5 -apple-system,system-ui,sans-serif;padding:24px;max-width:560px}"
      "h1{color:#5cc8ff;font-size:20px}code{background:#0b0f14;padding:2px 6px;border-radius:4px}</style>"
      "</head><body>"
      "<h1>Wi-Fi saved</h1>"
      "<p>The device is rebooting. On boot, it scans visible networks and joins "
      "the saved one with the strongest signal — likely <b>" + safeSsid + "</b> "
      "if it's in range. The <i>WemosRFID</i> hotspot will disappear; your phone "
      "may switch back to its previous network automatically.</p>"
      "<p>Once joined, find the device at <code>http://wemosrfid.local</code> "
      "(macOS, iOS, Linux, Windows 10+) or <code>\\\\WEMOSRFID</code> from "
      "Windows Explorer. On Android, look at your router's DHCP leases.</p>"
      "<p>If no saved network is reachable, the device falls back to the "
      "<i>WemosRFID</i> hotspot after about 15 seconds — reconnect and try again.</p>"
      "</body></html>";
  } else {
    body =
      "<!doctype html><html><head><meta charset=utf-8>"
      "<meta name=viewport content='width=device-width,initial-scale=1'>"
      "<title>WemosRFID — Wi-Fi saved</title>"
      "<meta http-equiv=refresh content='2;url=/'>"
      "<style>body{margin:0;background:#0e1116;color:#e6e6e6;font:15px/1.5 -apple-system,system-ui,sans-serif;padding:24px;max-width:560px}"
      "h1{color:#5cc8ff;font-size:20px}a{color:#5cc8ff}</style>"
      "</head><body>"
      "<h1>Wi-Fi saved</h1>"
      "<p><b>" + safeSsid + "</b> has been added to the saved networks list. "
      "The current connection is unchanged — the new network will be tried at "
      "the next boot, alongside the others, with the strongest signal winning.</p>"
      "<p>Returning to <a href=/>controls</a>…</p>"
      "</body></html>";
  }
  server.send(200, "text/html", body);
  if (reboot) {
    delay(500);
    ESP.restart();
  }
}

// ---- Main loop card processing --------------------------------------------
void processCard() {
  if (!mfrc.PICC_IsNewCardPresent() || !mfrc.PICC_ReadCardSerial()) return;

  // Always refresh "current card" info regardless of mode
  readCardSummary();
  Serial.printf("[RFID] card: UID=%s SAK=0x%02X type='%s' (mode=%d)\n",
                op.lastUidHex.c_str(), mfrc.uid.sak,
                op.lastCardType.c_str(), (int)op.mode);

  switch (op.mode) {
    case MODE_IDLE:
      op.lastResult = "Card detected: " + op.lastUidHex;
      break;

    case MODE_DUMP: {
      auto t = mfrc.PICC_GetType(mfrc.uid.sak);
      if (t == MFRC522::PICC_TYPE_MIFARE_1K || t == MFRC522::PICC_TYPE_MIFARE_4K ||
          t == MFRC522::PICC_TYPE_MIFARE_MINI) {
        op.lastDump = dumpClassic1K();
        op.lastResult = "Dump complete.";
        // Embed dump into next status response by tucking into result
        // (UI fetches dump via the same /api/cmd response if requested; here we
        //  return inline via a follow-up status — keep it simple: send through cmd)
      } else {
        Serial.printf("[DUMP] skipped: card type '%s' not MIFARE Classic\n",
                      op.lastCardType.c_str());
        op.lastResult = "Dump only supported for MIFARE Classic.";
      }
      op.mode = MODE_IDLE;
    } break;

    case MODE_CLONE_READ_SRC:
      memcpy(op.pendingUid, mfrc.uid.uidByte, mfrc.uid.size);
      op.pendingUidLen = mfrc.uid.size;
      Serial.printf("[CLONE] source captured: UID=%s (%u bytes), now waiting for TARGET\n",
                    op.lastUidHex.c_str(), op.pendingUidLen);
      op.lastResult = "Source UID " + op.lastUidHex + " captured. Now place TARGET (magic) card.";
      op.mode = MODE_CLONE_WRITE_DST;
      break;

    case MODE_CLONE_WRITE_DST:
    case MODE_WRITE_UID: {
      const char* tag = (op.mode == MODE_CLONE_WRITE_DST) ? "CLONE" : "WRITE-UID";
      if (op.pendingUidLen != 4) {
        Serial.printf("[%s] aborted: pending UID is %u bytes, need 4 for magic card\n",
                      tag, op.pendingUidLen);
        op.lastResult = "Magic-card UID write requires a 4-byte UID.";
        op.mode = MODE_IDLE;
        break;
      }
      Serial.printf("[%s] writing UID %s to target (current UID %s)\n",
                    tag,
                    bytesToHex(op.pendingUid, op.pendingUidLen).c_str(),
                    op.lastUidHex.c_str());
      bool ok = mfrc.MIFARE_SetUid(op.pendingUid, op.pendingUidLen, true);
      if (ok) {
        Serial.printf("[%s] OK — UID %s written\n",
                      tag, bytesToHex(op.pendingUid, op.pendingUidLen).c_str());
        op.lastResult = "UID written: " + bytesToHex(op.pendingUid, op.pendingUidLen) +
                        ". Re-tap to verify.";
      } else {
        Serial.printf("[%s] FAILED — likely not a magic Gen1A card\n", tag);
        op.lastResult = "Write failed. Card likely not a magic Gen1A card.";
      }
      op.mode = MODE_IDLE;
    } break;

    case MODE_WRITE_BLOCK: {
      if (writeBlockClassic(op.pendingBlock, op.pendingData)) {
        op.lastResult = "Block " + String(op.pendingBlock) + " written.";
      } else {
        op.lastResult = "Block write failed (auth or write error).";
      }
      op.mode = MODE_IDLE;
    } break;
  }

  mfrc.PICC_HaltA();
  mfrc.PCD_StopCrypto1();
}

// ---- Setup / Loop ----------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(50);
  Serial.println();
  Serial.println(F("[WemosRFID] booting"));

  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH); // off; pattern starts after netMode is decided

  SPI.begin();
  mfrc.PCD_Init();
  delay(20);
  mfrc.PCD_DumpVersionToSerial();

  if (!LittleFS.begin()) {
    Serial.println(F("[FS] mount failed, formatting..."));
    LittleFS.format();
    LittleFS.begin();
  }
  syncWifiCache();
  migrateSavedJsonIfPresent();
  syncSavedCache();

  WiFi.persistent(false);

  setLedMode(LED_CONNECTING);
  if (tryConnectAny()) {
    netMode = NET_STATION;
    setLedMode(LED_STATION);
    Serial.printf("[STA] joined '%s' IP=%s\n",
                  staSSID.c_str(), WiFi.localIP().toString().c_str());
  } else {
    Serial.println(F("[STA] no saved networks reachable, falling back to setup AP"));
    WiFi.disconnect(false);
    // AP_STA (not pure AP) so handleWifiScan() can use the STA radio while we
    // host the setup hotspot.
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(AP_SSID, AP_PASS);
    netMode = NET_SETUP_AP;
    setLedMode(LED_AP);
    Serial.printf("[AP] SSID=%s IP=%s\n", AP_SSID, WiFi.softAPIP().toString().c_str());
  }

  if (MDNS.begin(MDNS_HOST)) {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("[mDNS] http://%s.local\n", MDNS_HOST);
  }
  NBNS.begin(MDNS_HOST);
  Serial.printf("[NetBIOS] \\\\%s\n", MDNS_HOST);

  server.on("/", handleRoot);
  server.on("/api/status",    HTTP_GET,  handleStatus);
  server.on("/api/cmd",       HTTP_POST, handleCmd);
  server.on("/api/wifi-scan",  HTTP_GET,  handleWifiScan);
  server.on("/api/wifi-saved", HTTP_GET,  handleWifiSaved);
  server.on("/api/wifi-save",  HTTP_POST, handleWifiSave);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println(F("[HTTP] server up on :80"));
}

void loop() {
  server.handleClient();
  MDNS.update();
  processCard();
  updateLed();
}
