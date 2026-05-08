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
 * Required libraries (Arduino IDE -> Library Manager):
 *   - "MFRC522" by GithubCommunity (miguelbalboa fork)
 *   - ESP8266WiFi, ESP8266WebServer, ESP8266mDNS, LittleFS (bundled w/ esp8266 core)
 *
 * Board: "LOLIN(WEMOS) D1 R2 & mini" in the ESP8266 boards manager.
 *
 * Usage: connect to Wi-Fi SSID "WemosRFID" (password "rfidtools"),
 * then browse to http://192.168.4.1 (or http://wemosrfid.local).
 */

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
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

// ---- Globals ---------------------------------------------------------------
MFRC522 mfrc(SS_PIN, RST_PIN);
ESP8266WebServer server(80);

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

  out += "UID:  " + bytesToHex(mfrc.uid.uidByte, mfrc.uid.size, true) + "\n";
  out += "SAK:  0x" + String(mfrc.uid.sak, HEX) + "\n";
  out += "Type: " + picctypeName(mfrc.PICC_GetType(mfrc.uid.sak)) + "\n\n";

  for (uint8_t sector = 0; sector < 16; sector++) {
    uint8_t trailer = sector * 4 + 3;
    auto status = mfrc.PCD_Authenticate(MFRC522::PICC_CMD_MF_AUTH_KEY_A, trailer, &key, &mfrc.uid);
    if (status != MFRC522::STATUS_OK) {
      out += "S" + String(sector) + ": auth failed (default key A)\n";
      continue;
    }
    for (uint8_t blk = sector * 4; blk <= trailer; blk++) {
      uint8_t buf[18]; uint8_t sz = sizeof(buf);
      auto rs = mfrc.MIFARE_Read(blk, buf, &sz);
      if (rs == MFRC522::STATUS_OK) {
        out += "  blk " + String(blk) + ": " + bytesToHex(buf, 16, true) + "\n";
      } else {
        out += "  blk " + String(blk) + ": read error\n";
      }
    }
  }
  mfrc.PICC_HaltA();
  mfrc.PCD_StopCrypto1();
  return out;
}

bool writeBlockClassic(uint8_t block, const uint8_t* data16) {
  MFRC522::MIFARE_Key key;
  for (uint8_t i = 0; i < 6; i++) key.keyByte[i] = 0xFF;
  uint8_t trailer = (block / 4) * 4 + 3;
  auto status = mfrc.PCD_Authenticate(MFRC522::PICC_CMD_MF_AUTH_KEY_A, trailer, &key, &mfrc.uid);
  if (status != MFRC522::STATUS_OK) return false;
  status = mfrc.MIFARE_Write(block, (uint8_t*)data16, 16);
  mfrc.PICC_HaltA();
  mfrc.PCD_StopCrypto1();
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
input[type=text]{background:#0b0f14;color:var(--fg);border:1px solid #2c3340;border-radius:6px;padding:8px;font-family:ui-monospace,Menlo,Consolas,monospace;font-size:14px;flex:1;min-width:160px}
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
  server.send_P(200, "text/html", INDEX_HTML);
}

void handleStatus() {
  bool present = (op.lastEventMs && (millis() - op.lastEventMs) < 1500);
  String json = "{";
  json += "\"present\":" + String(present ? "true" : "false");
  json += ",\"uid\":\"" + op.lastUidHex + "\"";
  json += ",\"cardType\":\"" + escJson(op.lastCardType) + "\"";
  json += ",\"status\":\"" + escJson(op.lastResult) + "\"";
  json += ",\"mode\":" + String((int)op.mode);
  json += ",\"ip\":\"" + WiFi.softAPIP().toString() + "\"";
  json += ",\"saved\":" + loadSaved();
  json += "}";
  sendJson(200, json);
}

void handleCmd() {
  if (!server.hasArg("plain")) { sendJson(400, "{\"ok\":false,\"message\":\"missing body\"}"); return; }
  String body = server.arg("plain");

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
    op.mode = MODE_IDLE;
    op.lastResult = "Place a card on the reader.";
    sendJson(200, "{\"ok\":true,\"message\":\"Place a card on the reader.\"}");
    return;
  }
  if (cmd == "dump") {
    op.mode = MODE_DUMP;
    op.lastResult = "Place a MIFARE Classic 1K card to dump.";
    sendJson(200, "{\"ok\":true,\"message\":\"Place a card to dump.\"}");
    return;
  }
  if (cmd == "save") {
    if (op.lastUidHex.length() == 0) {
      sendJson(200, "{\"ok\":false,\"message\":\"No UID read yet.\"}"); return;
    }
    String saved = loadSaved();
    if (saved.indexOf("\"" + op.lastUidHex + "\"") >= 0) {
      sendJson(200, "{\"ok\":false,\"message\":\"Already saved.\"}"); return;
    }
    if (saved == "[]") saved = "[\"" + op.lastUidHex + "\"]";
    else { saved.remove(saved.length() - 1); saved += ",\"" + op.lastUidHex + "\"]"; }
    writeSaved(saved);
    sendJson(200, "{\"ok\":true,\"message\":\"Saved.\"}");
    return;
  }
  if (cmd == "forget") {
    int idx = field("index").toInt();
    String saved = loadSaved();
    int count = 0, start = -1, end = -1, depth = 0;
    for (size_t i = 0; i < saved.length(); i++) {
      if (saved[i] == '"') {
        if (depth == 0) { if (count == idx) start = i; depth = 1; }
        else { depth = 0; if (count == idx) { end = i; break; } count++; }
      }
    }
    if (start >= 0 && end > start) {
      String before = saved.substring(0, start);
      String after  = saved.substring(end + 1);
      // strip the comma separator on whichever side
      if (before.endsWith(",")) before.remove(before.length() - 1);
      else if (after.startsWith(",")) after.remove(0, 1);
      writeSaved(before + after);
    }
    sendJson(200, "{\"ok\":true,\"message\":\"Removed.\"}");
    return;
  }
  if (cmd == "clone-start") {
    op.mode = MODE_CLONE_READ_SRC;
    op.lastResult = "Place SOURCE card to read its UID.";
    sendJson(200, "{\"ok\":true,\"message\":\"Place SOURCE card.\"}");
    return;
  }
  if (cmd == "cancel") {
    op.mode = MODE_IDLE;
    op.pendingUidLen = 0;
    op.lastResult = "Cancelled.";
    sendJson(200, "{\"ok\":true,\"message\":\"Cancelled.\"}");
    return;
  }
  if (cmd == "write-uid") {
    String uid = field("uid");
    uint8_t buf[10]; uint8_t len = 0;
    if (!hexToBytes(uid, buf, sizeof(buf), len) || (len != 4 && len != 7)) {
      sendJson(200, "{\"ok\":false,\"message\":\"UID must be 4 or 7 hex bytes.\"}"); return;
    }
    memcpy(op.pendingUid, buf, len);
    op.pendingUidLen = len;
    op.mode = MODE_WRITE_UID;
    op.lastResult = "Place magic card to write UID " + bytesToHex(buf, len) + ".";
    sendJson(200, "{\"ok\":true,\"message\":\"Place magic card.\"}");
    return;
  }
  if (cmd == "write-block") {
    int blk = field("block").toInt();
    String data = field("data");
    uint8_t buf[16]; uint8_t len = 0;
    if (!hexToBytes(data, buf, sizeof(buf), len) || len != 16) {
      sendJson(200, "{\"ok\":false,\"message\":\"Data must be exactly 16 bytes.\"}"); return;
    }
    if (blk < 1 || blk > 62 || blk % 4 == 3) {
      sendJson(200, "{\"ok\":false,\"message\":\"Block must be 1..62 and not a sector trailer.\"}"); return;
    }
    op.pendingBlock = blk;
    memcpy(op.pendingData, buf, 16);
    op.mode = MODE_WRITE_BLOCK;
    op.lastResult = "Place card to write block " + String(blk) + ".";
    sendJson(200, "{\"ok\":true,\"message\":\"Place card.\"}");
    return;
  }

  sendJson(200, "{\"ok\":false,\"message\":\"Unknown command\"}");
}

void handleNotFound() {
  server.send(404, "text/plain", "Not found");
}

// ---- Main loop card processing --------------------------------------------
void processCard() {
  if (!mfrc.PICC_IsNewCardPresent() || !mfrc.PICC_ReadCardSerial()) return;

  // Always refresh "current card" info regardless of mode
  readCardSummary();

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
        op.lastResult = "Dump only supported for MIFARE Classic.";
      }
      op.mode = MODE_IDLE;
    } break;

    case MODE_CLONE_READ_SRC:
      memcpy(op.pendingUid, mfrc.uid.uidByte, mfrc.uid.size);
      op.pendingUidLen = mfrc.uid.size;
      op.lastResult = "Source UID " + op.lastUidHex + " captured. Now place TARGET (magic) card.";
      op.mode = MODE_CLONE_WRITE_DST;
      break;

    case MODE_CLONE_WRITE_DST:
    case MODE_WRITE_UID: {
      if (op.pendingUidLen != 4) {
        op.lastResult = "Magic-card UID write requires a 4-byte UID.";
        op.mode = MODE_IDLE;
        break;
      }
      bool ok = mfrc.MIFARE_SetUid(op.pendingUid, op.pendingUidLen, true);
      if (ok) {
        op.lastResult = "UID written: " + bytesToHex(op.pendingUid, op.pendingUidLen) +
                        ". Re-tap to verify.";
      } else {
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
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  Serial.print(F("[AP] SSID=")); Serial.print(AP_SSID);
  Serial.print(F(" IP=")); Serial.println(WiFi.softAPIP());

  if (MDNS.begin(MDNS_HOST)) {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("[mDNS] http://%s.local\n", MDNS_HOST);
  }

  server.on("/", handleRoot);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/cmd", HTTP_POST, handleCmd);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println(F("[HTTP] server up on :80"));
}

void loop() {
  server.handleClient();
  MDNS.update();
  processCard();
}
