#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <Update.h>
#include <FS.h>
#define private public
#include <FFat.h>
#undef private
#include <USB.h>
#include <USBMSC.h>
#include "tusb.h"
#include <wear_levelling.h>
#include <esp_partition.h>
#include <diskio_wl.h>
#include <diskio.h>
#include <SPI.h>
#include <SD.h>
#include "drop32_logo.h"

static const char *VERSION = "V1.0.0";
static const char *AP_SSID = "DROP32-SETUP";
static const char *AP_PASSWORD = "drop32setup";
static const char *WEB_PASSWORD = "drop32";

WebServer server(80);
Preferences preferences;
String configuredWifiSsid;
bool updateOk = false;
String updateError;
bool rebootPending = false;
uint32_t rebootAtMs = 0;
File uploadFile;
bool uploadOk = false;
String uploadError;
String uploadTarget;
fs::FS *activeStorage = &FFat;
bool sdReady = false;
bool usingSD = false;
bool usbDriveMode = false;
bool usbUsesSD = false;
volatile bool hostEjectRequested = false;
USBMSC MSC;
uint16_t mscBlockSize = 512;
BYTE internalPdrv = 0xFF;
static uint8_t mscScratch[4096];
portMUX_TYPE transferMux = portMUX_INITIALIZER_UNLOCKED;
uint32_t transferStartedMs = 0;
uint32_t transferLastMs = 0;
uint32_t transferBytes = 0;
uint32_t transferSpeedBps = 0;
uint8_t transferMedium = 0;  // 1 = internal, 2 = SD
uint8_t transferMethod = 0;  // 1 = web, 2 = USB

static void recordTransfer(uint32_t bytes, bool usbMethod) {
  const uint32_t now = millis();
  const uint8_t medium = (usbMethod ? usbUsesSD : usingSD) ? 2 : 1;
  const uint8_t method = usbMethod ? 2 : 1;
  portENTER_CRITICAL(&transferMux);
  if (!transferStartedMs || now - transferLastMs > 2000 || transferMedium != medium || transferMethod != method) {
    transferStartedMs = now;
    transferBytes = 0;
    transferSpeedBps = 0;
    transferMedium = medium;
    transferMethod = method;
  }
  transferBytes += bytes;
  transferLastMs = now;
  const uint32_t elapsed = now - transferStartedMs;
  if (elapsed) transferSpeedBps = (uint32_t)(((uint64_t)transferBytes * 1000ULL) / elapsed);
  portEXIT_CRITICAL(&transferMux);
}

static void resetTransferMeasurement(bool usbMethod) {
  portENTER_CRITICAL(&transferMux);
  transferStartedMs = millis();
  transferLastMs = transferStartedMs;
  transferBytes = 0;
  transferSpeedBps = 0;
  transferMedium = (usbMethod ? usbUsesSD : usingSD) ? 2 : 1;
  transferMethod = usbMethod ? 2 : 1;
  portEXIT_CRITICAL(&transferMux);
}

static void finalizeTransferMeasurement() {
  const uint32_t now = millis();
  portENTER_CRITICAL(&transferMux);
  if (transferBytes) {
    const uint32_t elapsed = max((uint32_t)1, now - transferStartedMs);
    transferSpeedBps = (uint32_t)(((uint64_t)transferBytes * 1000ULL) / elapsed);
    transferLastMs = now;
  }
  portEXIT_CRITICAL(&transferMux);
}

static String transferJsonFields() {
  uint32_t speed;
  uint8_t medium, method;
  portENTER_CRITICAL(&transferMux);
  speed = transferSpeedBps;
  medium = transferMedium;
  method = transferMethod;
  portEXIT_CRITICAL(&transferMux);
  String target = medium == 2 ? "SD Card" : (medium == 1 ? "Internal Flash" : "—");
  String via = method == 2 ? "USB" : (method == 1 ? "Web" : "—");
  return "\"transferSpeed\":" + String(speed) + ",\"transferTarget\":\"" + target + "\",\"transferMethod\":\"" + via + "\"";
}

static bool internalDiskRead(uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t size) {
  if (internalPdrv == 0xFF || mscBlockSize > sizeof(mscScratch)) return false;
  while (size) {
    const uint32_t amount = min(size, (uint32_t)mscBlockSize - offset);
    if (offset == 0 && amount == mscBlockSize) {
      if (disk_read(internalPdrv, buffer, lba, 1) != RES_OK) return false;
    } else {
      if (disk_read(internalPdrv, mscScratch, lba, 1) != RES_OK) return false;
      memcpy(buffer, mscScratch + offset, amount);
    }
    buffer += amount;
    size -= amount;
    ++lba;
    offset = 0;
  }
  return true;
}

static bool internalDiskWrite(uint32_t lba, uint32_t offset, const uint8_t *buffer, uint32_t size) {
  if (internalPdrv == 0xFF || mscBlockSize > sizeof(mscScratch)) return false;
  while (size) {
    const uint32_t amount = min(size, (uint32_t)mscBlockSize - offset);
    if (offset == 0 && amount == mscBlockSize) {
      if (disk_write(internalPdrv, buffer, lba, 1) != RES_OK) return false;
    } else {
      if (disk_read(internalPdrv, mscScratch, lba, 1) != RES_OK) return false;
      memcpy(mscScratch + offset, buffer, amount);
      if (disk_write(internalPdrv, mscScratch, lba, 1) != RES_OK) return false;
    }
    buffer += amount;
    size -= amount;
    ++lba;
    offset = 0;
  }
  return true;
}

static int32_t mscRead(uint32_t lba, uint32_t offset, void *buffer, uint32_t size) {
  if (!usbDriveMode) return -1;
  if (!usbUsesSD) {
    const bool ok = internalDiskRead(lba, offset, static_cast<uint8_t *>(buffer), size);
    if (!ok) Serial.printf("MSC READ ERROR: LBA %u, offset %u, size %u\n", lba, offset, size);
    return ok ? size : -1;
  }
  if (offset || size % 512) return -1;
  uint8_t *out = static_cast<uint8_t *>(buffer);
  for (uint32_t i = 0; i < size / 512; ++i) if (!SD.readRAW(out + i * 512, lba + i)) return -1;
  return size;
}

static int32_t mscWrite(uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t size) {
  if (!usbDriveMode) return -1;
  if (!usbUsesSD) {
    const bool ok = internalDiskWrite(lba, offset, buffer, size);
    if (!ok) Serial.printf("MSC WRITE ERROR: LBA %u, offset %u, size %u\n", lba, offset, size);
    if (ok) recordTransfer(size, true);
    return ok ? size : -1;
  }
  if (offset || size % 512) return -1;
  for (uint32_t i = 0; i < size / 512; ++i) if (!SD.writeRAW(buffer + i * 512, lba + i)) return -1;
  recordTransfer(size, true);
  return size;
}

static bool mscStartStop(uint8_t, bool start, bool loadEject) {
  if (loadEject && !start) hostEjectRequested = true;
  return true;
}

static void configureMSC(uint32_t sectors, uint16_t blockSize) {
  MSC.end();
  MSC.vendorID("DROP32");
  MSC.productID("OFFLINE DRIVE");
  MSC.productRevision("2.0");
  MSC.onRead(mscRead); MSC.onWrite(mscWrite); MSC.onStartStop(mscStartStop);
  mscBlockSize = blockSize;
  MSC.isWritable(true); MSC.begin(sectors, blockSize); MSC.mediaPresent(false);
}

static size_t activeTotalBytes() { return usingSD ? SD.totalBytes() : FFat.totalBytes(); }
static size_t activeUsedBytes() { return usingSD ? SD.usedBytes() : FFat.usedBytes(); }
static const char *activeStorageName() { return usingSD ? "SD Card" : "Internal FAT Drive"; }

static String jsonEscape(const String &value) {
  String out;
  out.reserve(value.length() + 8);
  for (size_t i = 0; i < value.length(); ++i) {
    const char c = value[i];
    if (c == '\\' || c == '"') { out += '\\'; out += c; }
    else if (c == '\n') out += "\\n";
    else if (c == '\r') out += "\\r";
    else if ((uint8_t)c >= 0x20) out += c;
  }
  return out;
}

static String cleanPath(String path, bool allowRoot = true) {
  path.trim();
  path.replace("\\", "/");
  if (!path.startsWith("/")) path = "/" + path;
  while (path.indexOf("//") >= 0) path.replace("//", "/");
  if (path.indexOf("..") >= 0) return "";
  while (path.length() > 1 && path.endsWith("/")) path.remove(path.length() - 1);
  if (!allowRoot && path == "/") return "";
  return path;
}

static String safeName(String name) {
  name.trim();
  name.replace("\\", "_");
  name.replace("/", "_");
  name.replace("..", "_");
  for (size_t i = 0; i < name.length(); ++i) if ((uint8_t)name[i] < 0x20) name.setCharAt(i, '_');
  return name;
}

static bool ensureParentFolders(const String &filePath) {
  int slash = filePath.indexOf('/', 1);
  while (slash > 0) {
    String folder = filePath.substring(0, slash);
    if (!activeStorage->exists(folder) && !activeStorage->mkdir(folder)) return false;
    slash = filePath.indexOf('/', slash + 1);
  }
  return true;
}

static bool authenticated() {
  return server.hasHeader("Cookie") && server.header("Cookie").indexOf("drop_session=ok") >= 0;
}

static bool requireAuth() {
  if (authenticated()) return true;
  server.send(401, "application/json", "{\"ok\":false,\"error\":\"Authentication required\"}");
  return false;
}

static bool requireWebStorage() {
  if (!requireAuth()) return false;
  if (!usbDriveMode) return true;
  server.send(409, "application/json", "{\"ok\":false,\"error\":\"Eject the drive on the computer, then switch to WEB TRANSFER MODE\"}");
  return false;
}

static bool deleteTree(const String &path) {
  File node = activeStorage->open(path);
  if (!node) return false;
  if (!node.isDirectory()) { node.close(); return activeStorage->remove(path); }
  File child = node.openNextFile();
  while (child) {
    String childPath = child.path();
    child.close();
    if (!deleteTree(childPath)) { node.close(); return false; }
    child = node.openNextFile();
  }
  node.close();
  return activeStorage->rmdir(path);
}

static void appendEntries(File &dir, String &json, bool &first) {
  File entry = dir.openNextFile();
  while (entry) {
    String path = entry.path();
    bool folder = entry.isDirectory();
    size_t size = folder ? 0 : entry.size();
    String name = path.substring(path.lastIndexOf('/') + 1);
    if (!first) json += ',';
    first = false;
    json += "{\"path\":\"" + jsonEscape(path) + "\",\"name\":\"" + jsonEscape(name) + "\",\"type\":\"" + (folder ? "folder" : "file") + "\",\"size\":" + String(size) + ",\"status\":\"READY\"}";
    if (folder) appendEntries(entry, json, first);
    entry.close();
    entry = dir.openNextFile();
  }
}

static String storageJson() {
  if (usbDriveMode) return "{\"ok\":true,\"version\":\"" + String(VERSION) + "\",\"storage\":\"" + activeStorageName() + "\",\"sdDetected\":" + (sdReady ? "true" : "false") + ",\"usbMode\":true,\"total\":0,\"used\":0,\"free\":0," + transferJsonFields() + ",\"entries\":[]}";
  String json;
  json.reserve(4096);
  const size_t total = activeTotalBytes();
  const size_t used = activeUsedBytes();
  if (total == 0) return "{\"ok\":false,\"error\":\"Selected storage is unavailable\"}";
  json = "{\"ok\":true,\"version\":\"" + String(VERSION) + "\",\"storage\":\"" + activeStorageName() + "\",\"sdDetected\":" + (sdReady ? "true" : "false") + ",\"usbMode\":false,\"total\":" + String(total) + ",\"used\":" + String(used) + ",\"free\":" + String(total >= used ? total - used : 0) + "," + transferJsonFields() + ",\"entries\":[";
  File root = activeStorage->open("/");
  bool first = true;
  if (root) { appendEntries(root, json, first); root.close(); }
  json += "]}";
  return json;
}

static String networkJson() {
  const wl_status_t status = WiFi.status();
  const bool connected = status == WL_CONNECTED;
  String state = "disconnected";
  if (connected) state = "connected";
  else if (configuredWifiSsid.length()) {
    if (status == WL_NO_SSID_AVAIL) state = "network not found";
    else if (status == WL_CONNECT_FAILED) state = "connection failed";
    else state = "connecting";
  }
  String ssid = connected ? WiFi.SSID() : configuredWifiSsid;
  return "{\"ok\":true,\"state\":\"" + state + "\",\"ssid\":\"" + jsonEscape(ssid) +
         "\",\"ip\":\"" + (connected ? WiFi.localIP().toString() : String("")) +
         "\",\"hostname\":\"drop32.local\",\"apSsid\":\"" + String(AP_SSID) +
         "\",\"apIp\":\"" + WiFi.softAPIP().toString() + "\"}";
}

static String networkScanJson() {
  const int count = WiFi.scanNetworks(false, true);
  if (count < 0) return "{\"ok\":false,\"error\":\"Wi-Fi scan failed\"}";
  String json = "{\"ok\":true,\"networks\":[";
  bool first = true;
  for (int i = 0; i < count; ++i) {
    const String ssid = WiFi.SSID(i);
    if (!ssid.length()) continue;
    bool duplicate = false;
    for (int j = 0; j < i; ++j) if (WiFi.SSID(j) == ssid) { duplicate = true; break; }
    if (duplicate) continue;
    if (!first) json += ',';
    first = false;
    json += "{\"ssid\":\"" + jsonEscape(ssid) + "\",\"rssi\":" + String(WiFi.RSSI(i)) +
            ",\"secure\":" + String(WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "false" : "true") + "}";
  }
  WiFi.scanDelete();
  json += "]}";
  return json;
}

static const char LOGIN_HTML[] PROGMEM = R"HTML(<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>D.R.O.P.32 Login</title><style>
*{box-sizing:border-box}body{margin:0;background:#071014;color:#d8f8ef;font:15px Arial,sans-serif;display:grid;place-items:center;min-height:100vh}.card{width:min(560px,92vw);border:1px solid #1c655b;background:#0b191d;padding:24px;box-shadow:0 0 32px #00ffc322}.logo{display:block;width:100%}.dropPoster{display:block;width:100%;height:auto}small{color:#7fa79f}input,button{width:100%;margin-top:18px;padding:13px;background:#071014;border:1px solid #2b7d70;color:#eafff9}button{background:#16b58c;color:#04110d;font-weight:bold;cursor:pointer}.err{color:#ff6d74;height:20px;margin-top:12px}
</style></head><body><form class="card" id="f"><div class="logo"><img class="dropPoster" src="/drop32-logo.png" alt="D.R.O.P.32 Wi-Fi to USB logo"></div><p>D.R.O.P.32 V1.0.0</p><small>Transfer files to offline computers via ESP32 and USB.<br>No LAN connection required. Safety first.</small><input id="p" type="password" placeholder="ACCESS PASSWORD" autofocus><button>ENTER D.R.O.P.32</button><div class="err" id="e"></div></form><script>f.onsubmit=async x=>{x.preventDefault();e.textContent='';let r=await fetch('/login',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'password='+encodeURIComponent(p.value)});if(r.ok)location='/';else e.textContent='ACCESS DENIED';}</script></body></html>)HTML";

static const char APP_HTML[] PROGMEM = R"HTML(<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>D.R.O.P.32 V1.0.0</title><style>
:root{--bg:#061014;--panel:#0b191d;--line:#1b5f56;--green:#48ffd1;--muted:#86aaa3;--amber:#ffcc52;--red:#ff6d74}*{box-sizing:border-box}body{margin:0;background:radial-gradient(circle at 80% 0,#10342f 0,transparent 32%),var(--bg);color:#dffbf4;font:14px Arial,sans-serif}header{padding:22px 4vw;border-bottom:1px solid var(--line)}.brand{font-size:28px;font-weight:900;letter-spacing:4px;color:var(--green)}.brand i{color:var(--amber);font-style:normal;transform:rotate(-8deg);display:inline-block}.sub{color:var(--muted);font-size:11px;letter-spacing:2px;margin-top:5px}nav{display:flex;gap:4px;padding:0 4vw;border-bottom:1px solid #123b36;overflow:auto}nav button{background:none;border:0;color:var(--muted);padding:16px 14px;font-weight:bold;cursor:pointer;white-space:nowrap}nav button.active{color:var(--green);border-bottom:2px solid var(--green)}main{max-width:1160px;margin:auto;padding:28px 4vw}.page{display:none}.page.active{display:block}.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:14px}.card{background:linear-gradient(145deg,#0d2024,#091619);border:1px solid #174b45;padding:20px}.label{color:var(--muted);font-size:11px;letter-spacing:1.5px}.value{font-size:24px;font-weight:bold;margin-top:9px}.ok{color:var(--green)}.drop{border:2px dashed #248c79;min-height:190px;display:grid;place-items:center;text-align:center;cursor:pointer;background:#0a1b1d;margin-bottom:20px}.drop.drag{border-color:var(--green);background:#10312c}.drop h1{letter-spacing:2px;color:var(--green);font-size:clamp(20px,4vw,34px)}.drop input{display:none}.progress{height:10px;background:#061014;border:1px solid #22544d;margin:14px 0}.bar{height:100%;width:0;background:var(--green);transition:width .15s}.message{min-height:24px;font-weight:bold}.message.good{color:var(--green)}.message.bad{color:var(--red)}.toolbar,.storage-head{display:flex;align-items:center;justify-content:space-between;gap:12px;flex-wrap:wrap}.storage-head{margin:28px 0 12px}.storage-head h2{margin:0;letter-spacing:2px}.usage{color:var(--muted)}button.action{border:1px solid #2d776c;background:#0a171a;color:#cffff3;padding:7px 10px;cursor:pointer}button.danger{border-color:#71353a;color:#ff9ba0}.entry{display:grid;grid-template-columns:minmax(180px,1fr) 100px 85px auto;align-items:center;gap:12px;padding:11px;border-bottom:1px solid #153a36}.entry:hover{background:#0d2324}.name{white-space:nowrap;overflow:hidden;text-overflow:ellipsis}.ready{color:var(--green);font-size:11px}.actions{display:flex;gap:5px;justify-content:flex-end;flex-wrap:wrap}.empty{color:var(--muted);padding:30px;text-align:center}.note{line-height:1.7;color:#b9d2cc}code{color:var(--amber)}@media(max-width:720px){.entry{grid-template-columns:1fr auto}.entry .size{display:none}.actions{grid-column:1/-1;justify-content:flex-start}}
.brand{display:flex;align-items:center}.brand .dropLogo{width:250px;max-width:75vw;height:auto;filter:drop-shadow(0 0 7px #48ffd133)}.switches{display:grid;grid-template-columns:repeat(2,minmax(260px,1fr));gap:14px;margin-bottom:30px}.switchbox{border:1px solid #246c62;background:#0a1a1d;padding:18px}.switchbox .label{margin-bottom:12px}.switchrow{display:grid;grid-template-columns:1fr 1fr;gap:8px}.modebtn{min-height:58px;border:1px solid #2d776c;background:#081418;color:#9dbbb5;padding:10px;font-weight:800;letter-spacing:.7px;cursor:pointer}.modebtn.active{border-color:var(--green);background:#12362f;color:var(--green);box-shadow:inset 0 0 0 1px #48ffd155,0 0 14px #48ffd11f}.modebtn:disabled:not(.active){opacity:.35;cursor:not-allowed}.switchhint{color:var(--amber);min-height:18px;margin-top:12px;font-size:12px}@media(max-width:720px){.switches{grid-template-columns:1fr}.switchrow{grid-template-columns:1fr 1fr}.modebtn{font-size:12px}}
.clickname{cursor:pointer;color:#dffbf4}.clickname:hover{color:var(--green);text-decoration:underline}.netgrid{display:grid;grid-template-columns:1fr 1fr;gap:16px}.netform label{display:block;color:var(--muted);margin:14px 0 6px}.netform input,.netform select{width:100%;padding:11px;border:1px solid #2d776c;background:#071014;color:#eafff9}.netactions{display:flex;gap:8px;margin-top:16px;flex-wrap:wrap}.netstatus{font-size:18px;font-weight:bold;color:var(--green)}.netmessage{min-height:20px;margin-top:12px;color:var(--amber)}.features{display:grid;grid-template-columns:repeat(auto-fit,minmax(240px,1fr));gap:14px}.features h2{color:var(--green);font-size:17px;margin-top:0}.features p{line-height:1.6;color:#b9d2cc}@media(max-width:720px){.netgrid{grid-template-columns:1fr}}
.updatebox{margin-top:16px}.updatebox input{display:block;width:100%;margin:14px 0;padding:11px;border:1px solid #2d776c;background:#071014;color:#eafff9}.updatebox .progress{max-width:620px}.updateMessage{min-height:22px;color:var(--amber);font-weight:bold}
</style></head><body><header><div class="brand"><img class="dropLogo" src="/drop32-logo.png" alt="D.R.O.P.32 Wi-Fi to USB logo"></div><div class="sub">TRANSFER FILES TO OFFLINE COMPUTERS · NO LAN REQUIRED · V1.0.0</div></header><nav id="nav"><button data-p="dashboard" class="active">DASHBOARD</button><button data-p="transfer">TRANSFER</button><button data-p="network">NETWORK</button><button data-p="system">SYSTEM</button><button data-p="help">HELP</button></nav><main>
<section id="dashboard" class="page active"><div class="switches"><div class="switchbox"><div class="label">ACTIVE STORAGE</div><div class="switchrow"><button class="modebtn" id="useInternal">INTERNAL FLASH</button><button class="modebtn" id="useSD">SD CARD</button></div><div class="switchhint" id="storageHint"></div></div><div class="switchbox"><div class="label">DRIVE ACCESS</div><div class="switchrow"><button class="modebtn" id="useWebMode">WEB TRANSFER</button><button class="modebtn" id="useUsbMode">USB DRIVE</button></div><div class="switchhint" id="modeHint"></div></div></div><h1>SYSTEM OVERVIEW</h1><div class="grid"><div class="card"><div class="label">STORAGE</div><div class="value" id="dStorage">—</div></div><div class="card"><div class="label">FILES</div><div class="value" id="dFiles">—</div></div><div class="card"><div class="label">USED</div><div class="value" id="dUsed">—</div></div><div class="card"><div class="label">FREE</div><div class="value" id="dFree">—</div></div><div class="card"><div class="label">OFFLINE COMPUTER</div><div class="value ok">ISOLATED ✓</div></div><div class="card"><div class="label">USB DRIVE</div><div class="value" id="dUsb">WEB MODE</div></div><div class="card"><div class="label">TRANSFER SPEED</div><div class="value" id="dSpeed">—</div><div class="label" id="dSpeedInfo">NO TRANSFER YET</div></div></div></section>
<section id="transfer" class="page"><div class="drop" id="drop"><div><h1>DROP YOUR FILES HERE</h1><p>or choose files from your device</p><button type="button" class="action" id="selectFiles">SELECT FILES</button> <button type="button" class="action" id="selectFolder">SELECT FOLDER</button><input type="file" id="picker" multiple hidden><input type="file" id="folderPicker" webkitdirectory directory multiple hidden><div class="progress"><div class="bar" id="bar"></div></div><div class="message" id="message"></div></div></div><div class="storage-head"><h2>FILES ON D.R.O.P.32</h2><div class="usage" id="usage">Loading storage…</div></div><div class="toolbar"><div>Current folder: <code id="folder">/</code></div><button class="action" id="newFolder">+ NEW FOLDER</button></div><div class="card" id="entries"></div></section>

<section id="network" class="page"><h1>NETWORK</h1><div class="netgrid"><div class="card note"><div class="label">PRIVATE SETUP ACCESS POINT</div><p><b>SSID:</b> DROP32-SETUP<br><b>Address:</b> 192.168.4.1<br><b>Internet routing:</b> Disabled<br><b>Status:</b> Always available</p></div><div class="card netform"><div class="label">YOUR WI-FI NETWORK</div><p class="netstatus" id="wifiState">CHECKING…</p><p id="wifiDetails">D.R.O.P.32 can also be reached through your own Wi-Fi.</p><label for="wifiSsid">Available 2.4 GHz Wi-Fi networks</label><select id="wifiSsid"><option value="">Select SCAN NETWORKS…</option></select><label for="wifiPassword">Wi-Fi password</label><input id="wifiPassword" type="password" maxlength="63" autocomplete="new-password"><div class="netactions"><button class="action" id="wifiScan">SCAN NETWORKS</button><button class="action" id="wifiConnect">CONNECT</button><button class="action danger" id="wifiDisconnect">DISCONNECT</button></div><div class="netmessage" id="wifiMessage"></div></div></div><div class="card note" style="margin-top:16px"><b>Important:</b> Only D.R.O.P.32 joins your modern Wi-Fi. The offline computer remains isolated and receives no network connection through USB.</div></section>
<section id="system" class="page"><h1>SYSTEM</h1><div class="card note"><p><b>Device:</b> ESP32-S3 DevKitC-1 N16R8<br><b>Firmware:</b> D.R.O.P.32 V1.0.0<br><b>Active storage:</b> <span id="systemStorage">—</span><br><b>SD card:</b> <span id="sdState">Checking…</span><br><b>Drive access:</b> <span id="driveMode">WEB TRANSFER MODE</span></p><p class="label">STORAGE AND DRIVE ACCESS ARE CONTROLLED FROM THE DASHBOARD.</p><p class="label">SD SPI PINS: CS 10 · MOSI 11 · SCK 12 · MISO 13</p></div><div class="card updatebox"><h2>FIRMWARE UPDATE</h2><p class="note">Select a D.R.O.P.32 <code>firmware.bin</code> file. Keep the device powered and do not close this page during the update. The device restarts automatically after verification.</p><input id="firmwareFile" type="file" accept=".bin,application/octet-stream"><button class="action" id="installFirmware">INSTALL UPDATE</button><div class="progress"><div class="bar" id="updateBar"></div></div><div class="updateMessage" id="updateMessage"></div></div></section>
<section id="help" class="page"><h1>D.R.O.P.32 FEATURES</h1><div class="features"><div class="card"><h2>WEB TRANSFER</h2><p>Upload individual files or complete folder structures from a modern browser. Download, rename, organize and delete them without installing software.</p></div><div class="card"><h2>USB DRIVE</h2><p>Present the selected FAT storage as a writable USB drive to macOS, Windows or an offline computer. The offline computer receives files without joining a network.</p></div><div class="card"><h2>INTERNAL FLASH + SD</h2><p>Use the built-in flash or an optional SD card. Both preserve folders, and the Dashboard shows used space, free space and transfer speed.</p></div><div class="card"><h2>ISOLATED OFFLINE COMPUTER</h2><p>The USB connection does not bridge Wi-Fi or Internet access to Windows XP/2000. This reduces exposure of unsupported systems to network attacks.</p></div><div class="card"><h2>PRIVATE SETUP AP</h2><p>Connect directly to <b>DROP32-SETUP</b> and open <b>192.168.4.1</b>. This recovery connection stays available even when another Wi-Fi configuration fails.</p></div><div class="card"><h2>YOUR OWN WI-FI</h2><p>Connecting D.R.O.P.32 to your 2.4 GHz home or workshop network makes its web interface available without changing Wi-Fi on your phone or Mac. Use the displayed IP address or <b>drop32.local</b>. Only D.R.O.P.32 joins that network; the offline computer remains isolated.</p></div></div><div class="card note" style="margin-top:16px"><b>Safe workflow:</b> Use either WEB TRANSFER or USB DRIVE for a storage medium, never both simultaneously. Always eject the USB drive before returning to WEB TRANSFER.</div></section>
</main><script>
const $=x=>document.getElementById(x);let current='/',updating=false;const fmt=n=>n<1024?n+' B':n<1048576?(n/1024).toFixed(1)+' KB':(n/1048576).toFixed(2)+' MB';
nav.onclick=e=>{if(!e.target.dataset.p)return;document.querySelectorAll('nav button,.page').forEach(x=>x.classList.remove('active'));e.target.classList.add('active');$(e.target.dataset.p).classList.add('active');refresh();if(e.target.dataset.p==='network'&&wifiSsid.options.length<2)scanNetworks()};
async function api(url,opt){let r=await fetch(url,opt),data;try{data=await r.json()}catch(_){data={ok:false,error:'Invalid server response'}}if(!r.ok||!data.ok)throw Error(data.error||('Request failed: '+r.status));return data}
async function refresh(){try{let d=await api('/api/files');let files=d.entries.filter(x=>x.type==='file'),internal=d.storage==='Internal FAT Drive';dStorage.textContent=d.storage;dFiles.textContent=d.usbMode?'—':files.length;dUsed.textContent=d.usbMode?'HOST':fmt(d.used);dFree.textContent=d.usbMode?'OWNS DRIVE':fmt(d.free);dUsb.textContent=d.usbMode?'USB ACTIVE':'WEB MODE';dSpeed.textContent=d.transferSpeed?(d.transferSpeed/1024).toFixed(1)+' KB/s':'—';dSpeedInfo.textContent=d.transferSpeed?d.transferMethod.toUpperCase()+' → '+d.transferTarget.toUpperCase():'NO TRANSFER YET';driveMode.textContent=d.usbMode?'USB DRIVE MODE':'WEB TRANSFER MODE';usage.textContent=d.usbMode?'USB DRIVE ACTIVE — EJECT BEFORE WEB MODE':d.storage+' · '+fmt(d.used)+' USED · '+fmt(d.free)+' FREE';systemStorage.textContent=d.storage;sdState.textContent=d.sdDetected?'DETECTED':'NOT DETECTED';useInternal.classList.toggle('active',internal);useSD.classList.toggle('active',!internal);useWebMode.classList.toggle('active',!d.usbMode);useUsbMode.classList.toggle('active',d.usbMode);useSD.disabled=!d.sdDetected||d.usbMode||!internal;useInternal.disabled=d.usbMode||internal;useWebMode.disabled=!d.usbMode;useUsbMode.disabled=d.usbMode;storageHint.textContent=d.usbMode?'Storage is locked while USB DRIVE is active.':(!d.sdDetected?'No SD card detected.':'');modeHint.textContent=d.usbMode?'USB DRIVE ACTIVE — eject it on the computer to return.':'Browser transfer and file management are active.';newFolder.disabled=d.usbMode;selectFiles.disabled=d.usbMode;selectFolder.disabled=d.usbMode;installFirmware.disabled=d.usbMode||updating;if(d.usbMode)entries.innerHTML='<div class="empty">Drive is controlled by the connected computer.</div>';else render(d.entries)}catch(e){entries.innerHTML='<div class="empty">'+esc(e.message)+'</div>';show(e.message,false)}}
function esc(s){return String(s).replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]))}
function parent(p){if(p==='/')return '/';let a=p.split('/');a.pop();return a.join('/')||'/'}
function render(all){folder.textContent=current;let shown=all.filter(x=>parent(x.path)===current).sort((a,b)=>(a.type===b.type?a.name.localeCompare(b.name):a.type==='folder'?-1:1));let h=current!=='/'?'<div class="entry"><div class="name clickname" onclick="go(\''+esc(parent(current))+'\')">↰ ..</div><div></div><div></div><div><button class="action" onclick="go(\''+esc(parent(current))+'\')">UP</button></div></div>':'';for(let x of shown){let p=encodeURIComponent(x.path).replace(/'/g,'%27'),folderAction=x.type==='folder'?' onclick="go(decodeURIComponent(\''+p+'\'))"':'';h+='<div class="entry"><div class="name '+(x.type==='folder'?'clickname':'')+'"'+folderAction+'>'+(x.type==='folder'?'📁 ':'📄 ')+esc(x.name)+'</div><div class="size">'+(x.type==='file'?fmt(x.size):'FOLDER')+'</div><div class="ready">✓ READY</div><div class="actions">'+(x.type==='folder'?'<button class="action" onclick="go(decodeURIComponent(\''+p+'\'))">OPEN</button>':'<button class="action" onclick="location=\'/api/download?path='+p+'\'">DOWNLOAD</button>')+'<button class="action" onclick="renameItem(decodeURIComponent(\''+p+'\'))">RENAME</button><button class="action danger" onclick="removeItem(decodeURIComponent(\''+p+'\'))">DELETE</button></div></div>'}entries.innerHTML=h||'<div class="empty">No files in this folder.</div>'}
function go(p){current=p;refresh()}
newFolder.onclick=async()=>{let name=prompt('New folder name');if(!name)return;try{await api('/api/folder',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'path='+encodeURIComponent((current==='/'?'':current)+'/'+name)});await refresh()}catch(e){show(e.message,false)}};
async function renameItem(path){let old=path.split('/').pop(),name=prompt('New name',old);if(!name||name===old)return;let to=parent(path);to=(to==='/'?'':to)+'/'+name;try{await api('/api/rename',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'from='+encodeURIComponent(path)+'&to='+encodeURIComponent(to)});await refresh()}catch(e){show(e.message,false)}}
async function removeItem(path){if(!confirm('Delete '+path+'?'))return;try{await api('/api/delete',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'path='+encodeURIComponent(path)});await refresh()}catch(e){show(e.message,false)}}
function show(t,good){message.textContent=t;message.className='message '+(good?'good':'bad')}
async function upload(files){if(!files||!files.length){show('No file selected',false);return}let count=files.length,index=0;for(let file of files){index++;let relative=file.webkitRelativePath||file.name,target=(current==='/'?'':current)+'/'+relative;show('Preparing '+relative+' ('+index+'/'+count+') …',true);bar.style.width='0%';let xhr=new XMLHttpRequest(),done=new Promise((res,rej)=>{xhr.timeout=120000;xhr.upload.onloadstart=()=>show('Uploading '+relative+' ('+index+'/'+count+') …',true);xhr.upload.onprogress=e=>{if(e.lengthComputable){let pc=Math.round(e.loaded/e.total*100);bar.style.width=pc+'%';show('Uploading '+relative+' — '+pc+'% ('+index+'/'+count+')',true)}};xhr.onload=()=>{let d;try{d=JSON.parse(xhr.responseText)}catch(_){d={ok:false,error:'Invalid server response ('+xhr.status+')'}};xhr.status>=200&&xhr.status<300&&d.ok?res(d):rej(Error(d.error||'Upload failed'))};xhr.onerror=()=>rej(Error('Network connection lost during upload'));xhr.ontimeout=()=>rej(Error('Upload timed out'))});xhr.open('POST','/api/upload');xhr.setRequestHeader('X-DROP-Path',target);let fd=new FormData();fd.append('file',file,file.name);xhr.send(fd);try{await done}catch(e){show('✕ '+relative+': '+e.message,false);return}}bar.style.width='100%';show('✓ '+count+' ITEM'+(count===1?'':'S')+' TRANSFERRED — READY FOR USB',true);picker.value='';folderPicker.value='';await refresh()}
async function chooseStorage(mode){try{show('Switching storage …',true);await api('/api/storage',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'mode='+mode});current='/';show('Storage switched',true);await refresh()}catch(e){show(e.message,false)}}
async function chooseDriveMode(mode){if(mode==='web'&&!confirm('Have you ejected the D.R.O.P.32 drive on the computer?'))return;try{show('Switching drive mode …',true);await api('/api/drive-mode',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'mode='+mode});show(mode==='usb'?'USB DRIVE MODE ACTIVE':'WEB TRANSFER MODE ACTIVE',true);await refresh()}catch(e){show(e.message,false)}}
async function refreshNetwork(){try{let n=await api('/api/network');wifiState.textContent=n.state.toUpperCase();wifiDisconnect.disabled=n.state==='disconnected';if(n.ssid&&!Array.from(wifiSsid.options).some(o=>o.value===n.ssid))wifiSsid.add(new Option(n.ssid+' (configured)',n.ssid));if(n.ssid&&!wifiSsid.matches(':focus'))wifiSsid.value=n.ssid;wifiDetails.textContent=n.state==='connected'?'Connected to '+n.ssid+' · '+n.ip+' · '+n.hostname:(n.state==='connecting'?'Trying to connect to '+n.ssid+' …':'Not connected to your Wi-Fi. Setup AP remains available.')}catch(e){wifiMessage.textContent=e.message}}
async function scanNetworks(){wifiScan.disabled=true;wifiMessage.textContent='Scanning 2.4 GHz networks …';let selected=wifiSsid.value;try{let d=await api('/api/network/scan');wifiSsid.innerHTML='';wifiSsid.add(new Option(d.networks.length?'Select a Wi-Fi network…':'No networks found',''));for(let n of d.networks)wifiSsid.add(new Option(n.ssid+' · '+n.rssi+' dBm'+(n.secure?' · secured':' · open'),n.ssid));if(selected&&Array.from(wifiSsid.options).some(o=>o.value===selected))wifiSsid.value=selected;wifiMessage.textContent=d.networks.length?d.networks.length+' network(s) found.':'No 2.4 GHz networks found.'}catch(e){wifiMessage.textContent=e.message}finally{wifiScan.disabled=false}}
wifiConnect.onclick=async()=>{let ssid=wifiSsid.value.trim();if(!ssid){wifiMessage.textContent='Enter a Wi-Fi name.';return}wifiMessage.textContent='Connecting …';try{await api('/api/network/connect',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'ssid='+encodeURIComponent(ssid)+'&password='+encodeURIComponent(wifiPassword.value)});wifiPassword.value='';wifiMessage.textContent='Connection started. Status updates automatically.';setTimeout(refreshNetwork,1500)}catch(e){wifiMessage.textContent=e.message}};
wifiDisconnect.onclick=async()=>{try{await api('/api/network/disconnect',{method:'POST'});wifiPassword.value='';wifiSsid.value='';wifiMessage.textContent='Disconnected. The setup access point remains active.';await refreshNetwork()}catch(e){wifiMessage.textContent=e.message}};
wifiScan.onclick=scanNetworks;
installFirmware.onclick=()=>{let file=firmwareFile.files[0];if(!file){updateMessage.textContent='Select a firmware.bin file first.';return}if(!file.name.toLowerCase().endsWith('.bin')){updateMessage.textContent='Only .bin firmware files are accepted.';return}if(!confirm('Install '+file.name+' and restart D.R.O.P.32?'))return;updating=true;installFirmware.disabled=true;firmwareFile.disabled=true;updateBar.style.width='0%';updateMessage.textContent='Uploading firmware …';let xhr=new XMLHttpRequest();xhr.timeout=180000;xhr.upload.onprogress=e=>{if(e.lengthComputable){let pc=Math.round(e.loaded/e.total*100);updateBar.style.width=pc+'%';updateMessage.textContent='Uploading firmware — '+pc+'%'}};xhr.onload=()=>{let d;try{d=JSON.parse(xhr.responseText)}catch(_){d={ok:false,error:'Invalid update response'}}if(xhr.status>=200&&xhr.status<300&&d.ok){updateBar.style.width='100%';updateMessage.textContent='UPDATE VERIFIED — RESTARTING D.R.O.P.32 …';setTimeout(()=>location.reload(),9000)}else{updating=false;installFirmware.disabled=false;firmwareFile.disabled=false;updateMessage.textContent='UPDATE FAILED: '+(d.error||xhr.status)}};xhr.onerror=()=>{updating=false;installFirmware.disabled=false;firmwareFile.disabled=false;updateMessage.textContent='UPDATE FAILED: connection lost'};xhr.ontimeout=()=>{updating=false;installFirmware.disabled=false;firmwareFile.disabled=false;updateMessage.textContent='UPDATE FAILED: timeout'};xhr.open('POST','/api/update');let data=new FormData();data.append('firmware',file,file.name);xhr.send(data)};
useWebMode.onclick=()=>chooseDriveMode('web');useUsbMode.onclick=()=>chooseDriveMode('usb');useInternal.onclick=()=>chooseStorage('internal');useSD.onclick=()=>chooseStorage('sd');selectFiles.onclick=e=>{e.stopPropagation();picker.click()};selectFolder.onclick=e=>{e.stopPropagation();folderPicker.click()};drop.ondragover=e=>{e.preventDefault();drop.classList.add('drag')};drop.ondragleave=()=>drop.classList.remove('drag');drop.ondrop=e=>{e.preventDefault();drop.classList.remove('drag');upload(Array.from(e.dataTransfer.files))};picker.onchange=()=>upload(Array.from(picker.files));folderPicker.onchange=()=>upload(Array.from(folderPicker.files));refresh();refreshNetwork();setInterval(()=>{refresh();refreshNetwork()},3000);
</script></body></html>)HTML";

static void sendJsonError(int status, const String &message) {
  server.send(status, "application/json", "{\"ok\":false,\"error\":\"" + jsonEscape(message) + "\"}");
}

static bool enterUsbMode(String &error) {
  if (usbDriveMode) return true;
  if (uploadFile) uploadFile.close();
  hostEjectRequested = false;
  usbUsesSD = usingSD;
  if (usbUsesSD) {
    uint32_t sectors = SD.numSectors();
    if (!sectors) { error = "SD card is not ready"; return false; }
    configureMSC(sectors, SD.sectorSize());
  } else {
    if (FFat._wl_handle == WL_INVALID_HANDLE) { error = "Internal FAT drive is not mounted"; return false; }
    internalPdrv = ff_diskio_get_pdrv_wl(FFat._wl_handle);
    if (internalPdrv == 0xFF) { error = "Internal FAT disk driver is unavailable"; return false; }
    uint16_t sectorSize = wl_sector_size(FFat._wl_handle);
    if (!sectorSize || sectorSize > sizeof(mscScratch)) { error = "Unsupported internal FAT sector size"; return false; }
    if (disk_ioctl(internalPdrv, CTRL_SYNC, nullptr) != RES_OK) { error = "Could not synchronize internal FAT drive"; return false; }
    configureMSC(wl_size(FFat._wl_handle) / sectorSize, sectorSize);
  }
  usbDriveMode = true;
  MSC.mediaPresent(true);
  tud_disconnect(); delay(300); tud_connect();
  return true;
}

static bool enterWebMode(String &error) {
  if (!usbDriveMode) return true;
  MSC.mediaPresent(false);
  tud_disconnect();
  delay(350);
  usbDriveMode = false;
  if (usbUsesSD) {
    SD.end();
    sdReady = SD.begin(10, SPI, 10000000) && SD.cardType() != CARD_NONE;
    if (!sdReady) { error = "SD card could not be remounted"; return false; }
    activeStorage = &SD;
  } else {
    if (internalPdrv != 0xFF) disk_ioctl(internalPdrv, CTRL_SYNC, nullptr);
    internalPdrv = 0xFF;
    FFat.end();
    if (!FFat.begin(false)) { error = "Internal FAT drive could not be remounted"; return false; }
    activeStorage = &FFat;
  }
  tud_connect();
  return true;
}

static void setupRoutes() {
  const char *headers[] = {"Cookie", "X-DROP-Path"};
  server.collectHeaders(headers, 2);
  server.on("/drop32-logo.png", HTTP_GET, []() {
    server.send_P(200, "image/png", reinterpret_cast<const char *>(DROP32_LOGO_PNG), DROP32_LOGO_PNG_LEN);
  });
  server.on("/login", HTTP_GET, []() { server.send_P(200, "text/html; charset=utf-8", LOGIN_HTML); });
  server.on("/login", HTTP_POST, []() {
    if (server.arg("password") != WEB_PASSWORD) { server.send(403, "text/plain", "Access denied"); return; }
    server.sendHeader("Set-Cookie", "drop_session=ok; Path=/; SameSite=Strict");
    server.send(200, "application/json", "{\"ok\":true}");
  });
  server.on("/", HTTP_GET, []() {
    if (!authenticated()) { server.sendHeader("Location", "/login"); server.send(302); return; }
    server.send_P(200, "text/html; charset=utf-8", APP_HTML);
  });
  server.on("/api/files", HTTP_GET, []() { if (!requireAuth()) return; server.send(200, "application/json", storageJson()); });
  server.on("/api/network", HTTP_GET, []() {
    if (!requireAuth()) return;
    server.send(200, "application/json", networkJson());
  });
  server.on("/api/network/scan", HTTP_GET, []() {
    if (!requireAuth()) return;
    String result = networkScanJson();
    server.send(result.indexOf("\"ok\":true") >= 0 ? 200 : 500, "application/json", result);
  });
  server.on("/api/network/connect", HTTP_POST, []() {
    if (!requireAuth()) return;
    String ssid = server.arg("ssid"), password = server.arg("password");
    ssid.trim();
    if (ssid.length() < 1 || ssid.length() > 32) { sendJsonError(400, "Wi-Fi name must contain 1 to 32 characters"); return; }
    if (password.length() > 0 && password.length() < 8) { sendJsonError(400, "Wi-Fi password must contain at least 8 characters"); return; }
    if (password.length() > 63) { sendJsonError(400, "Wi-Fi password is too long"); return; }
    configuredWifiSsid = ssid;
    preferences.putString("ssid", ssid);
    preferences.putString("password", password);
    WiFi.disconnect(false, false);
    WiFi.begin(ssid.c_str(), password.length() ? password.c_str() : nullptr);
    server.send(202, "application/json", "{\"ok\":true,\"state\":\"connecting\"}");
  });
  server.on("/api/network/disconnect", HTTP_POST, []() {
    if (!requireAuth()) return;
    configuredWifiSsid = "";
    preferences.remove("ssid");
    preferences.remove("password");
    WiFi.disconnect(false, true);
    server.send(200, "application/json", "{\"ok\":true,\"state\":\"disconnected\"}");
  });
  server.on("/api/drive-mode", HTTP_POST, []() {
    if (!requireAuth()) return;
    String error;
    bool ok = server.arg("mode") == "usb" ? enterUsbMode(error) : enterWebMode(error);
    if (!ok) { sendJsonError(500, error); return; }
    server.send(200, "application/json", "{\"ok\":true,\"usbMode\":" + String(usbDriveMode ? "true" : "false") + "}");
  });
  server.on("/api/storage", HTTP_POST, []() {
    if (!requireWebStorage()) return;
    String mode = server.arg("mode");
    if (mode == "internal") { activeStorage = &FFat; usingSD = false; }
    else if (mode == "sd" && sdReady) { activeStorage = &SD; usingSD = true; }
    else { sendJsonError(409, "SD card is not detected"); return; }
    server.send(200, "application/json", "{\"ok\":true}");
  });
  server.on("/api/download", HTTP_GET, []() {
    if (!requireWebStorage()) return;
    String path = cleanPath(server.arg("path"), false);
    if (path == "" || !activeStorage->exists(path)) { sendJsonError(404, "File not found"); return; }
    File file = activeStorage->open(path, FILE_READ);
    if (!file || file.isDirectory()) { if (file) file.close(); sendJsonError(400, "Not a file"); return; }
    String name = path.substring(path.lastIndexOf('/') + 1);
    server.sendHeader("Content-Disposition", "attachment; filename=\"" + name + "\"");
    server.streamFile(file, "application/octet-stream");
    file.close();
  });
  server.on("/api/folder", HTTP_POST, []() {
    if (!requireWebStorage()) return;
    String path = cleanPath(server.arg("path"), false);
    if (path == "") { sendJsonError(400, "Invalid folder path"); return; }
    if (activeStorage->exists(path)) { sendJsonError(409, "Folder already exists"); return; }
    if (!activeStorage->mkdir(path)) { sendJsonError(500, "Could not create folder"); return; }
    server.send(200, "application/json", "{\"ok\":true}");
  });
  server.on("/api/rename", HTTP_POST, []() {
    if (!requireWebStorage()) return;
    String from = cleanPath(server.arg("from"), false), to = cleanPath(server.arg("to"), false);
    if (from == "" || to == "") { sendJsonError(400, "Invalid path"); return; }
    if (!activeStorage->exists(from)) { sendJsonError(404, "Source not found"); return; }
    if (activeStorage->exists(to)) { sendJsonError(409, "Destination already exists"); return; }
    if (!activeStorage->rename(from, to)) { sendJsonError(500, "Rename failed"); return; }
    server.send(200, "application/json", "{\"ok\":true}");
  });
  server.on("/api/delete", HTTP_POST, []() {
    if (!requireWebStorage()) return;
    String path = cleanPath(server.arg("path"), false);
    if (path == "" || !activeStorage->exists(path)) { sendJsonError(404, "Item not found"); return; }
    if (!deleteTree(path)) { sendJsonError(500, "Delete failed"); return; }
    server.send(200, "application/json", "{\"ok\":true}");
  });
  server.on("/api/upload", HTTP_POST, []() {
    if (!requireAuth()) return;
    if (uploadFile) uploadFile.close();
    if (!uploadOk) { sendJsonError(500, uploadError.length() ? uploadError : "Upload was not stored"); return; }
    server.send(200, "application/json", "{\"ok\":true,\"path\":\"" + jsonEscape(uploadTarget) + "\"}");
  }, []() {
    if (!authenticated()) return;
    HTTPUpload &u = server.upload();
    if (u.status == UPLOAD_FILE_START) {
      uploadOk = false; uploadError = ""; uploadTarget = "";
      if (usbDriveMode) { uploadError = "Switch to WEB TRANSFER MODE before uploading"; return; }
      resetTransferMeasurement(false);
      uploadTarget = cleanPath(server.header("X-DROP-Path"), false);
      if (uploadTarget == "") { uploadError = "Invalid upload path"; return; }
      if (!ensureParentFolders(uploadTarget)) { uploadError = "Could not create folder structure"; return; }
      uploadFile = activeStorage->open(uploadTarget, FILE_WRITE);
      if (!uploadFile) uploadError = "Cannot open file in selected storage";
    } else if (u.status == UPLOAD_FILE_WRITE) {
      if (uploadFile) {
        const size_t written = uploadFile.write(u.buf, u.currentSize);
        if (written) recordTransfer(written, false);
        if (written != u.currentSize) uploadError = "Storage write failed";
      }
    } else if (u.status == UPLOAD_FILE_END) {
      if (uploadFile) { uploadFile.flush(); uploadFile.close(); }
      finalizeTransferMeasurement();
      if (uploadError == "" && activeStorage->exists(uploadTarget)) uploadOk = true;
      else if (uploadError == "") uploadError = "Server could not verify the stored file";
    } else if (u.status == UPLOAD_FILE_ABORTED) {
      if (uploadFile) uploadFile.close();
      if (uploadTarget.length()) activeStorage->remove(uploadTarget);
      uploadError = "Upload aborted";
    }
  });
  server.on("/api/update", HTTP_POST, []() {
    if (!requireAuth()) return;
    if (!updateOk) { sendJsonError(500, updateError.length() ? updateError : "Firmware update was not installed"); return; }
    server.send(200, "application/json", "{\"ok\":true,\"restarting\":true}");
    rebootPending = true;
    rebootAtMs = millis() + 1500;
  }, []() {
    if (!authenticated()) return;
    HTTPUpload &u = server.upload();
    if (u.status == UPLOAD_FILE_START) {
      updateOk = false;
      updateError = "";
      String filename = u.filename;
      filename.toLowerCase();
      if (usbDriveMode) { updateError = "Eject USB DRIVE and switch to WEB TRANSFER before updating"; return; }
      if (!filename.endsWith(".bin")) { updateError = "Only .bin firmware files are accepted"; return; }
      if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) updateError = String("Could not start update: ") + Update.errorString();
    } else if (u.status == UPLOAD_FILE_WRITE) {
      if (updateError.length()) return;
      if (Update.write(u.buf, u.currentSize) != u.currentSize) updateError = String("Firmware write failed: ") + Update.errorString();
    } else if (u.status == UPLOAD_FILE_END) {
      if (updateError.length()) { Update.abort(); return; }
      if (!Update.end(true) || !Update.isFinished()) updateError = String("Firmware verification failed: ") + Update.errorString();
      else updateOk = true;
    } else if (u.status == UPLOAD_FILE_ABORTED) {
      Update.abort();
      updateError = "Firmware upload was aborted";
    }
  });
  server.onNotFound([]() { if (!authenticated()) { server.sendHeader("Location", "/login"); server.send(302); } else sendJsonError(404, "Not found"); });
}

void setup() {
  Serial.begin(115200);
  delay(250);
  Serial.printf("\nD.R.O.P.32 %s\n", VERSION);
  preferences.begin("drop32", false);
  configuredWifiSsid = preferences.getString("ssid", "");
  String configuredWifiPassword = preferences.getString("password", "");
  WiFi.mode(WIFI_AP_STA);
  WiFi.setHostname("drop32");
  if (!WiFi.softAP(AP_SSID, AP_PASSWORD)) Serial.println("ERROR: Access point failed");
  else Serial.printf("AP ready: %s at %s\n", AP_SSID, WiFi.softAPIP().toString().c_str());
  if (configuredWifiSsid.length()) {
    WiFi.begin(configuredWifiSsid.c_str(), configuredWifiPassword.length() ? configuredWifiPassword.c_str() : nullptr);
    Serial.printf("Connecting to saved Wi-Fi: %s\n", configuredWifiSsid.c_str());
  }
  if (MDNS.begin("drop32")) MDNS.addService("http", "tcp", 80);
  if (!FFat.begin(false)) {
    Serial.println("FAT mount failed; formatting internal D.R.O.P.32 drive...");
    if (!FFat.begin(true)) Serial.println("ERROR: Internal FAT drive unavailable");
  }
  Serial.printf("Internal FAT Drive: %u used / %u bytes\n", (unsigned)FFat.usedBytes(), (unsigned)FFat.totalBytes());
  SPI.begin(12, 13, 11, 10);
  sdReady = SD.begin(10, SPI, 10000000) && SD.cardType() != CARD_NONE;
  if (sdReady) Serial.printf("SD Card detected: %u MB\n", (unsigned)(SD.cardSize() / 1048576ULL));
  else Serial.println("SD Card: not detected (CS 10, MOSI 11, SCK 12, MISO 13)");
  if (FFat._wl_handle != WL_INVALID_HANDLE) {
    uint16_t sectorSize = wl_sector_size(FFat._wl_handle);
    configureMSC(wl_size(FFat._wl_handle) / sectorSize, sectorSize);
  }
  USB.begin();
  setupRoutes();
  server.begin();
  Serial.println("Web UI ready: http://192.168.4.1");
  Serial.println("USB Mass Storage ready; select USB DRIVE on the DASHBOARD");
}

void loop() {
  server.handleClient();
  if (usbDriveMode && hostEjectRequested) {
    hostEjectRequested = false;
    String error;
    delay(500);
    if (enterWebMode(error)) Serial.println("Host ejected D.R.O.P.32; WEB TRANSFER MODE restored");
    else Serial.printf("ERROR after host eject: %s\n", error.c_str());
  }
  if (rebootPending && (int32_t)(millis() - rebootAtMs) >= 0) {
    Serial.println("Firmware update installed; restarting D.R.O.P.32...");
    delay(100);
    ESP.restart();
  }
  delay(2);
}
