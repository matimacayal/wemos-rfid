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
const char*  SAVED_FILE      = "/saved.json";
const char*  WIFI_FILE       = "/wifi.txt";  // line 1: SSID, line 2: password
constexpr uint32_t STA_CONNECT_TIMEOUT_MS = 15000;

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
String loadSaved() {
  if (!LittleFS.exists(SAVED_FILE)) return "[]";
  File f = LittleFS.open(SAVED_FILE, "r");
  if (!f) return "[]";
  String s = f.readString();
  f.close();
  if (s.length() == 0) return "[]";
  return s;
}

void writeSaved(const String& json) {
  File f = LittleFS.open(SAVED_FILE, "w");
  if (!f) return;
  f.print(json);
  f.close();
}

// ---- Wi-Fi credentials store ----------------------------------------------
bool loadWifiCreds(String& ssid, String& pass) {
  if (!LittleFS.exists(WIFI_FILE)) {
    Serial.println(F("[WiFi] no saved creds"));
    return false;
  }
  File f = LittleFS.open(WIFI_FILE, "r");
  if (!f) {
    Serial.println(F("[WiFi] failed to open creds file"));
    return false;
  }
  ssid = f.readStringUntil('\n'); ssid.trim();
  pass = f.readStringUntil('\n'); pass.trim();
  f.close();
  if (ssid.length() == 0) {
    Serial.println(F("[WiFi] creds file empty"));
    return false;
  }
  Serial.printf("[WiFi] loaded creds for SSID '%s' (pass len=%u)\n",
                ssid.c_str(), (unsigned)pass.length());
  return true;
}

void saveWifiCreds(const String& ssid, const String& pass) {
  File f = LittleFS.open(WIFI_FILE, "w");
  if (!f) {
    Serial.println(F("[WiFi] failed to open creds file for write"));
    return;
  }
  f.println(ssid);
  f.println(pass);
  f.close();
  Serial.printf("[WiFi] credentials saved for '%s' (pass len=%u)\n",
                ssid.c_str(), (unsigned)pass.length());
}

void clearWifiCreds() {
  LittleFS.remove(WIFI_FILE);
  Serial.println(F("[WiFi] credentials cleared"));
}

bool tryConnectSTA(const String& ssid, const String& pass) {
  WiFi.mode(WIFI_STA);
  WiFi.hostname(MDNS_HOST);
  WiFi.begin(ssid.c_str(), pass.c_str());
  Serial.printf("[STA] connecting to '%s' (timeout %lu ms)", ssid.c_str(),
                (unsigned long)STA_CONNECT_TIMEOUT_MS);
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < STA_CONNECT_TIMEOUT_MS) {
    delay(250);
    Serial.print('.');
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

  <div class="card" id="wifiCard">
    <h2>Network</h2>
    <div id="wifiSetup" style="display:none">
      <div class="muted">Join your Wi-Fi so the device is reachable on your normal network. The <b>WemosRFID</b> hotspot will turn off after connecting.</div>
      <form method="POST" action="/api/wifi-save" style="margin-top:8px">
        <div class="row">
          <input list="ssidList" name="ssid" placeholder="SSID" autocomplete="off" required style="flex:1"/>
          <button type="button" onclick="scanWifi()">Scan</button>
        </div>
        <datalist id="ssidList"></datalist>
        <div class="row" style="margin-top:6px">
          <input id="passIn" name="pass" type="password" placeholder="password (blank for open networks)" style="flex:1"/>
          <button type="button" id="passToggle" onclick="togglePassVis()" title="Show/hide password" aria-label="Show password">show</button>
          <button class="primary" type="submit">Save &amp; connect</button>
        </div>
      </form>
      <div class="warn" style="margin-top:8px">After saving, the device reboots. Find it at <code>http://wemosrfid.local</code> on the joined network (Windows, macOS, iOS, Linux). On Android, check your router's DHCP leases.</div>
    </div>
    <div id="wifiConnected" style="display:none">
      <div>SSID: <span id="staSSID" class="kv"></span></div>
      <div>IP:&nbsp;&nbsp; <span id="staIP" class="kv"></span></div>
      <div class="row" style="margin-top:8px">
        <button class="danger" onclick="forgetWifi()">Forget Wi-Fi (back to setup)</button>
      </div>
    </div>
  </div>

  <div class="card">
    <h2>Read</h2>
    <div class="row">
      <button class="primary" onclick="cmd('read')">Read UID</button>
      <button onclick="cmd('dump')">Full Memory Dump (MIFARE 1K)</button>
      <button onclick="cmd('save')">Save UID to library</button>
    </div>
    <pre id="dump" style="display:none"></pre>
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

  <div id="flash"></div>

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
async function forgetWifi(){
  if(!confirm('Forget Wi-Fi credentials and reboot? You will need to reconnect to the WemosRFID hotspot.')) return;
  await api('/api/cmd', {cmd:'wifi-forget'});
  flash('Forgotten. Device is rebooting — reconnect to the WemosRFID hotspot.');
}
let _scannedOnce = false;
async function refresh(){
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
    (s.saved||[]).forEach((u,i)=>{
      const li = document.createElement('li');
      li.innerHTML = '<span class="kv">'+u+'</span>';
      const right = document.createElement('span');
      const useBtn = document.createElement('button');
      useBtn.textContent = 'Load to writer';
      useBtn.onclick = ()=>{ document.getElementById('uidIn').value = u; };
      const delBtn = document.createElement('button');
      delBtn.textContent = '✕'; delBtn.className='danger';
      delBtn.onclick = async ()=>{ await api('/api/cmd',{cmd:'forget',index:i}); refresh(); };
      right.appendChild(useBtn); right.appendChild(delBtn);
      li.appendChild(right);
      list.appendChild(li);
    });
    document.getElementById('savedEmpty').style.display = (s.saved && s.saved.length) ? 'none' : 'block';
    const setupEl = document.getElementById('wifiSetup');
    const connEl  = document.getElementById('wifiConnected');
    if (s.netMode === 'station') {
      setupEl.style.display = 'none';
      connEl.style.display  = 'block';
      document.getElementById('staSSID').textContent = s.staSSID || '';
      document.getElementById('staIP').textContent   = s.staIP || s.ip || '';
    } else {
      setupEl.style.display = 'block';
      connEl.style.display  = 'none';
      if (!_scannedOnce) { _scannedOnce = true; scanWifi(); }
    }
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
  json += ",\"saved\":" + loadSaved();
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
    String saved = loadSaved();
    if (saved.indexOf("\"" + op.lastUidHex + "\"") >= 0) {
      Serial.printf("[CMD] save: UID %s already in library\n", op.lastUidHex.c_str());
      sendJson(200, "{\"ok\":false,\"message\":\"Already saved.\"}"); return;
    }
    if (saved == "[]") saved = "[\"" + op.lastUidHex + "\"]";
    else { saved.remove(saved.length() - 1); saved += ",\"" + op.lastUidHex + "\"]"; }
    writeSaved(saved);
    Serial.printf("[CMD] save: UID %s added to library\n", op.lastUidHex.c_str());
    sendJson(200, "{\"ok\":true,\"message\":\"Saved.\"}");
    return;
  }
  if (cmd == "forget") {
    int idx = field("index").toInt();
    Serial.printf("[CMD] forget index=%d\n", idx);
    String saved = loadSaved();
    int count = 0, start = -1, end = -1, depth = 0;
    for (size_t i = 0; i < saved.length(); i++) {
      if (saved[i] == '"') {
        if (depth == 0) { if (count == idx) start = i; depth = 1; }
        else { depth = 0; if (count == idx) { end = i; break; } count++; }
      }
    }
    if (start >= 0 && end > start) {
      String removed = saved.substring(start + 1, end);
      String before = saved.substring(0, start);
      String after  = saved.substring(end + 1);
      // strip the comma separator on whichever side
      if (before.endsWith(",")) before.remove(before.length() - 1);
      else if (after.startsWith(",")) after.remove(0, 1);
      writeSaved(before + after);
      Serial.printf("[CMD] forget: removed UID %s\n", removed.c_str());
    } else {
      Serial.printf("[CMD] forget: index %d not found\n", idx);
    }
    sendJson(200, "{\"ok\":true,\"message\":\"Removed.\"}");
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
    Serial.println(F("[CMD] wifi-forget: clearing creds and restarting"));
    clearWifiCreds();
    sendJson(200, "{\"ok\":true,\"message\":\"Wi-Fi forgotten. Rebooting.\"}");
    delay(400);
    ESP.restart();
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
  // Returns a JSON array — single element today (we only persist one set of
  // creds), but shaped for future multi-network support. Includes the password
  // in plaintext, consistent with the rest of the unauthenticated API.
  Serial.printf("[HTTP] GET /api/wifi-saved from %s\n",
                server.client().remoteIP().toString().c_str());
  String ssid, pass;
  String json = "[";
  if (loadWifiCreds(ssid, pass)) {
    json += "{\"ssid\":\"" + escJson(ssid) + "\",";
    json += "\"pass\":\"" + escJson(pass) + "\"}";
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
  saveWifiCreds(ssid, pass);
  Serial.printf("[WiFi] reboot to join '%s'\n", ssid.c_str());
  String safeSsid = htmlEscape(ssid);
  String body =
    "<!doctype html><html><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>WemosRFID — Wi-Fi saved</title>"
    "<style>body{margin:0;background:#0e1116;color:#e6e6e6;font:15px/1.5 -apple-system,system-ui,sans-serif;padding:24px;max-width:560px}"
    "h1{color:#5cc8ff;font-size:20px}code{background:#0b0f14;padding:2px 6px;border-radius:4px}</style>"
    "</head><body>"
    "<h1>Wi-Fi saved</h1>"
    "<p>The device is rebooting and will join <b>" + safeSsid + "</b>. The "
    "<i>WemosRFID</i> hotspot will disappear in a few seconds — your phone may "
    "switch back to its previous network automatically.</p>"
    "<p>Once joined, find the device at <code>http://wemosrfid.local</code> "
    "(macOS, iOS, Linux, Windows 10+) or <code>\\\\WEMOSRFID</code> from "
    "Windows Explorer. On Android, look at your router's DHCP leases.</p>"
    "<p>If the credentials are wrong, the device will fall back to the "
    "<i>WemosRFID</i> hotspot after about 15 seconds — reconnect and try again.</p>"
    "</body></html>";
  server.send(200, "text/html", body);
  delay(500);
  ESP.restart();
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

  SPI.begin();
  mfrc.PCD_Init();
  delay(20);
  mfrc.PCD_DumpVersionToSerial();

  if (!LittleFS.begin()) {
    Serial.println(F("[FS] mount failed, formatting..."));
    LittleFS.format();
    LittleFS.begin();
  }

  WiFi.persistent(false);

  String savedSsid, savedPass;
  if (loadWifiCreds(savedSsid, savedPass) && tryConnectSTA(savedSsid, savedPass)) {
    netMode = NET_STATION;
    staSSID = savedSsid;
    Serial.printf("[STA] joined '%s' IP=%s\n",
                  savedSsid.c_str(), WiFi.localIP().toString().c_str());
  } else {
    if (savedSsid.length()) Serial.println(F("[STA] saved creds failed, falling back to setup AP"));
    WiFi.disconnect(false);
    // AP_STA (not pure AP) so handleWifiScan() can use the STA radio while we
    // host the setup hotspot.
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(AP_SSID, AP_PASS);
    netMode = NET_SETUP_AP;
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
}
