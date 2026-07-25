#include "WebApp.h"
#include "Settings.h"
#include "AppState.h"
#include "Net.h"
#include "HuckTheme.h"

#include <WebServer.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include <Update.h>
#include <time.h>

namespace web {

#define FW_VERSION "v0.2.0-b2"   // bumped to verify OTA over Wi-Fi

static WebServer server(80);

// ---------------------------------------------------------------- page (SPA)
static const char PAGE[] PROGMEM = R"HTML(<!doctype html><html><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Huckleberry</title><style>
:root{--bg:#0b0805;--panel:#171008;--acc:#F4791F;--acc2:#8f7bd6;--tx:#cbb38c;--hi:#ffe9c8}
*{box-sizing:border-box}body{margin:0;font-family:system-ui,-apple-system,sans-serif;background:var(--bg);color:var(--tx)}
header{padding:16px 18px;display:flex;align-items:center;gap:10px}
header b{color:var(--acc);letter-spacing:3px;font-size:18px}
.flower{width:20px;height:20px;border-radius:50%;background:var(--acc2);box-shadow:0 -11px var(--acc),0 11px var(--acc),-11px 0 var(--acc),11px 0 var(--acc)}
.wrap{max-width:680px;margin:0 auto;padding:0 14px 40px}
.card{background:var(--panel);border-radius:16px;padding:16px 18px;margin:12px 0;box-shadow:0 4px 18px #0007}
.card h2{margin:0 0 12px;font-size:13px;letter-spacing:2px;color:var(--acc);text-transform:uppercase}
.big{font-size:44px;color:var(--hi);font-weight:700;line-height:1}
.row{display:flex;justify-content:space-between;gap:10px;margin:6px 0}
.k{color:#8a7c63}.v{color:var(--hi)}
.grid{display:grid;grid-template-columns:1fr 1fr;gap:10px}
button{background:#241a0d;color:var(--hi);border:1px solid #3a2a12;border-radius:10px;padding:10px 14px;font-size:16px;cursor:pointer}
button.acc{background:var(--acc);color:#1a0f00;border:0}
input,select{width:100%;padding:9px;border-radius:9px;border:1px solid #3a2a12;background:#0f0a04;color:var(--hi)}
label{display:block;font-size:12px;color:#8a7c63;margin:8px 0 3px}
.pill{display:inline-block;padding:3px 9px;border-radius:20px;background:#241a0d;font-size:12px}
.spin{margin-left:6px;font-size:44px;color:var(--hi)}
.thermo{display:flex;align-items:center;justify-content:center;gap:14px}
details summary{cursor:pointer;color:var(--acc);letter-spacing:2px;font-size:13px;text-transform:uppercase}
.ok{color:#7db56a}.bad{color:#e0725a}
</style></head><body>
<header><span class="flower"></span><b>HUCKLEBERRY</b><span id="clk" style="margin-left:auto;color:var(--hi);font-variant-numeric:tabular-nums"></span></header>
<div class="wrap">
 <div class="card"><h2>Power</h2>
  <div class="grid">
   <div><div class="k">Battery</div><div class="big"><span id="soc">--</span>%</div>
     <div class="row"><span class="k">Voltage</span><span class="v" id="bv">--</span></div>
     <div class="row"><span class="k">Current</span><span class="v" id="ba">--</span></div>
     <div class="k" id="battname">Eco-Worthy 280Ah</div></div>
   <div><div class="k">Solar</div><div class="big"><span id="pv">--</span>W</div>
     <div class="row"><span class="k">State</span><span class="v" id="ss">--</span></div>
     <div class="row"><span class="k">Yield today</span><span class="v" id="yd">--</span></div>
     <div class="k">Victron MPPT 75/15</div></div>
  </div></div>
 <div class="card"><h2>Climate</h2>
  <div class="thermo"><button onclick="sp(-1)">−</button>
    <div style="text-align:center"><div class="big"><span id="set">70</span>°</div><div class="k">set point</div></div>
    <button onclick="sp(1)">+</button></div>
  <div class="grid" style="margin-top:12px">
   <div><label>Mode</label><select id="mode" onchange="save()">
     <option value=0>Auto</option><option value=1>Cool</option><option value=2>Heat</option><option value=3>Fan</option><option value=4>Off</option></select></div>
   <div><label>Preset</label><select id="camp" onchange="save()"><option value=1>Camping</option><option value=0>Storing</option></select></div>
  </div>
  <div class="k" style="margin-top:8px">Gidrox 10k BTU · <span id="gidrox">not yet paired</span></div></div>
 <div class="card"><h2>Status</h2>
  <div class="row"><span class="k">Wi-Fi</span><span class="v" id="wifi">--</span></div>
  <div class="row"><span class="k">SSID</span><span class="v" id="ssid">--</span></div>
  <div class="row"><span class="k">IP</span><span class="v" id="ip">--</span></div>
  <div class="row"><span class="k">AP</span><span class="v" id="ap">--</span></div>
  <div class="row"><span class="k">Time source</span><span class="v" id="tsrc">--</span></div>
  <div class="row"><span class="k">Battery link</span><span class="v" id="blink">--</span></div>
  <div class="row"><span class="k">Solar link</span><span class="v" id="slink">--</span></div>
  <div class="row"><span class="k">Firmware</span><span class="v">v0.2.0-b1</span></div></div>
 <div class="card"><details><summary>Settings</summary>
  <label>Saved Wi-Fi networks (home + campsites)</label>
  <div id="netlist" class="k">none yet</div>
  <div class="grid" style="margin-top:8px">
   <input id="fssid" placeholder="SSID"><input id="fpass" type="password" placeholder="password">
  </div>
  <button class="acc" style="margin-top:8px" onclick="addNet()">Add network</button>
  <p class="k">The trailer auto-joins the first saved network it finds; the <b>Huckleberry</b> hotspot stays on for off-grid.</p>
  <label style="margin-top:14px">Day theme</label><select id="theme" onchange="save()"></select>
  <label>Day brightness</label><input id="bright" type="range" min="10" max="255" oninput="save()">
  <label style="margin-top:8px"><input type="checkbox" id="anim" style="width:auto" onchange="save()"> Animations</label>
  <label style="margin-top:14px">Night brightness</label><input id="nbright" type="range" min="5" max="150" oninput="save()">
  <label style="margin-top:8px"><input type="checkbox" id="autoNight" style="width:auto" onchange="save()"> Auto night mode (black face, white digits) on the home page</label>
  <div class="grid"><div><label>Night starts</label><select id="nStart" onchange="save()"></select></div>
   <div><label>Night ends</label><select id="nEnd" onchange="save()"></select></div></div>
  <label>Return-to-clock timeout (seconds)</label><input id="hto" type="number" min="10" max="600" onchange="save()">
  <label style="margin-top:10px"><input type="checkbox" id="doEn" style="width:auto" onchange="save()"> Turn the screen off on a schedule (wakes on touch)</label>
  <div class="grid"><div><label>Off from</label><select id="doStart" onchange="save()"></select></div>
   <div><label>Off until</label><select id="doEnd" onchange="save()"></select></div></div>
  <label style="margin-top:10px">Storing protection (°F) — used in Storing mode</label>
  <div class="grid"><div><label>Heat below</label><input id="stMin" type="number" min="20" max="60" onchange="save()"></div>
   <div><label>Cool above</label><input id="stMax" type="number" min="70" max="110" onchange="save()"></div></div>
  <label style="margin-top:12px">Firmware</label>
  <a href="/update"><button class="acc" style="width:100%">Update firmware (OTA)</button></a>
  <p class="k" style="margin-top:8px">BLE device discovery coming next.</p>
 </details></div>
</div>
<script>
let S={};
function $(i){return document.getElementById(i)}
function fmt(x,d,u){return (x==null||isNaN(x))?'--':(x.toFixed(d)+(u||''))}
async function load(){try{S=await (await fetch('/api/state')).json();paint()}catch(e){}}
function paint(){
 $('soc').textContent=S.batt&&S.batt.soc>=0?S.batt.soc:'--';
 $('bv').textContent=fmt(S.batt&&S.batt.v,2,' V');$('ba').textContent=fmt(S.batt&&S.batt.a,1,' A');
 $('pv').textContent=S.sol&&S.sol.valid?Math.round(S.sol.pv):'--';
 $('ss').textContent=S.sol?S.sol.state:'--';$('yd').textContent=fmt(S.sol&&S.sol.yield,2,' kWh');
 $('set').textContent=S.th.sp;$('mode').value=S.th.mode;$('camp').value=S.th.camp?1:0;
 $('wifi').innerHTML=S.net.sta?'<span class=ok>connected</span>':'<span class=bad>offline</span>';
 $('ssid').textContent=S.net.ssid||'--';$('ip').textContent=S.net.ip||'--';
 $('ap').textContent=S.net.ap||'--';$('tsrc').textContent=S.net.tsrc;
 $('blink').innerHTML=S.batt&&S.batt.valid?'<span class=ok>live</span>':'waiting…';
 $('slink').innerHTML=S.sol&&S.sol.valid?'<span class=ok>live ('+S.sol.rssi+'dBm)</span>':'waiting…';
 if(!themeInit){
   $('theme').innerHTML=S.themes.map((n,i)=>`<option value=${i}>${n}</option>`).join('');
   let hrs='';for(let h=0;h<24;h++){let ap=h<12?'AM':'PM',hh=h%12||12;hrs+=`<option value=${h}>${hh} ${ap}</option>`}
   ['nStart','nEnd','doStart','doEnd'].forEach(id=>$(id).innerHTML=hrs);
   themeInit=1}
 let s=S.set;
 $('theme').value=s.theme;$('bright').value=s.bright;$('anim').checked=s.anim;
 $('nbright').value=s.nbright;$('autoNight').checked=s.autoNight;
 $('nStart').value=s.nStart;$('nEnd').value=s.nEnd;$('hto').value=s.hto;
 $('doEn').checked=s.doEn;$('doStart').value=s.doStart;$('doEnd').value=s.doEnd;
 $('stMin').value=s.stMin;$('stMax').value=s.stMax;
 renderNets();
 if(S.time){$('clk').textContent=S.time}
}
let themeInit=0;
function renderNets(){
 let saved=(S.net&&S.net.saved)||[];
 if(!saved.length){$('netlist').textContent='none yet';return}
 $('netlist').innerHTML=saved.map(s=>{
   let cur=s===S.net.ssid?' <span class=ok>(connected)</span>':'';
   return `<div class=row><span class=v>${s}${cur}</span><button onclick="rmNet('${s.replace(/'/g,"\\'")}')">remove</button></div>`}).join('')
}
function post(u,b){return fetch(u,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:b})}
function sp(d){S.th.sp=Math.max(45,Math.min(90,(S.th.sp|0)+d));$('set').textContent=S.th.sp;save()}
function save(){post('/api/settings',`sp=${$('set').textContent}&mode=${$('mode').value}&camp=${$('camp').value}&theme=${$('theme').value}&bright=${$('bright').value}&anim=${$('anim').checked?1:0}`+
 `&nbright=${$('nbright').value}&autoNight=${$('autoNight').checked?1:0}&nStart=${$('nStart').value}&nEnd=${$('nEnd').value}&hto=${$('hto').value}`+
 `&doEn=${$('doEn').checked?1:0}&doStart=${$('doStart').value}&doEnd=${$('doEnd').value}&stMin=${$('stMin').value}&stMax=${$('stMax').value}`)}
function addNet(){let s=$('fssid').value;if(!s)return;post('/api/wifi/add',`ssid=${encodeURIComponent(s)}&pass=${encodeURIComponent($('fpass').value)}`).then(()=>{$('fssid').value='';$('fpass').value='';setTimeout(load,500)})}
function rmNet(s){post('/api/wifi/remove',`ssid=${encodeURIComponent(s)}`).then(()=>setTimeout(load,300))}
function pushTime(){post('/api/time','epoch='+Math.floor(Date.now()/1000))}
pushTime();load();setInterval(load,2000);setInterval(pushTime,60000);
</script></body></html>)HTML";

// ---------------------------------------------------------------- handlers
static const char* solStateName(uint8_t s) {
  switch (s) { case 0: return "Off"; case 3: return "Bulk"; case 4: return "Absorption";
    case 5: return "Float"; case 7: return "Equalize"; default: return "On"; }
}

static void handleState() {
  JsonDocument d;
  teleLock();
  Telemetry t = gTele;
  teleUnlock();
  auto b = d["batt"].to<JsonObject>();
  b["valid"] = t.battValid; b["soc"] = t.battSoc;
  if (!isnan(t.battVolts)) b["v"] = t.battVolts;
  if (!isnan(t.battAmps))  b["a"] = t.battAmps;
  auto s = d["sol"].to<JsonObject>();
  s["valid"] = t.solValid; s["pv"] = isnan(t.solPvW) ? 0.0f : t.solPvW;
  s["state"] = solStateName(t.solState);
  if (!isnan(t.solYieldKwh)) s["yield"] = t.solYieldKwh;
  s["rssi"] = t.solRssi;
  auto th = d["th"].to<JsonObject>();
  th["sp"] = gSettings.setpointF; th["mode"] = gSettings.mode; th["camp"] = gSettings.camping;
  auto n = d["net"].to<JsonObject>();
  n["sta"] = gNet.staConnected; n["ssid"] = gNet.ssid; n["ip"] = gNet.ip;
  n["ap"] = gNet.apActive ? gNet.apSsid : String("");
  n["tsrc"] = gNet.timeSource;
  auto saved = n["saved"].to<JsonArray>();
  for (auto& w : gSettings.networks) saved.add(w.ssid);
  auto st = d["set"].to<JsonObject>();
  st["theme"] = gSettings.dayThemeIdx; st["bright"] = gSettings.dayBrightness; st["anim"] = gSettings.animations;
  st["nbright"] = gSettings.nightBrightness; st["autoNight"] = gSettings.autoNight;
  st["nStart"] = gSettings.nightStartHour; st["nEnd"] = gSettings.nightEndHour;
  st["hto"] = gSettings.homeTimeoutSec;
  st["doEn"] = gSettings.dispOffEnable; st["doStart"] = gSettings.dispOffStartHour; st["doEnd"] = gSettings.dispOffEndHour;
  st["stMin"] = gSettings.storeMinF; st["stMax"] = gSettings.storeMaxF;
  auto themes = d["themes"].to<JsonArray>();
  for (size_t i = 0; i < HUCK_THEME_COUNT; i++) themes.add(HUCK_THEMES[i].name);
  d["fw"] = FW_VERSION;
  // wall clock
  if (net::timeIsValid()) {
    time_t now = time(nullptr); struct tm lt; localtime_r(&now, &lt);
    char buf[16]; int h = lt.tm_hour; const char* ap = h < 12 ? "AM" : "PM";
    int h12 = h % 12; if (!h12) h12 = 12;
    snprintf(buf, sizeof(buf), gSettings.use24h ? "%02d:%02d" : "%d:%02d %s",
             gSettings.use24h ? h : h12, lt.tm_min, ap);
    d["time"] = buf;
  }
  String out; serializeJson(d, out);
  server.send(200, "application/json", out);
}

static void handleSettings() {
  if (server.hasArg("sp")) gSettings.setpointF = server.arg("sp").toInt();
  if (server.hasArg("mode")) gSettings.mode = server.arg("mode").toInt();
  if (server.hasArg("camp")) gSettings.camping = server.arg("camp").toInt() != 0;
  if (server.hasArg("theme")) gSettings.dayThemeIdx = server.arg("theme").toInt();
  if (server.hasArg("bright")) gSettings.dayBrightness = server.arg("bright").toInt();
  if (server.hasArg("anim")) gSettings.animations = server.arg("anim").toInt() != 0;
  if (server.hasArg("nbright")) gSettings.nightBrightness = server.arg("nbright").toInt();
  if (server.hasArg("autoNight")) gSettings.autoNight = server.arg("autoNight").toInt() != 0;
  if (server.hasArg("nStart")) gSettings.nightStartHour = server.arg("nStart").toInt();
  if (server.hasArg("nEnd")) gSettings.nightEndHour = server.arg("nEnd").toInt();
  if (server.hasArg("hto")) gSettings.homeTimeoutSec = server.arg("hto").toInt();
  if (server.hasArg("doEn")) gSettings.dispOffEnable = server.arg("doEn").toInt() != 0;
  if (server.hasArg("doStart")) gSettings.dispOffStartHour = server.arg("doStart").toInt();
  if (server.hasArg("doEnd")) gSettings.dispOffEndHour = server.arg("doEnd").toInt();
  if (server.hasArg("stMin")) gSettings.storeMinF = server.arg("stMin").toInt();
  if (server.hasArg("stMax")) gSettings.storeMaxF = server.arg("stMax").toInt();
  gSettings.save();
  gUiApplyRequested = true;
  server.send(200, "text/plain", "ok");
}

static void handleWifiAdd() {
  String ssid = server.arg("ssid");
  if (ssid.length()) {
    gSettings.addNetwork(ssid, server.arg("pass"));
    gSettings.save();
    net::reconnect();
  }
  server.send(200, "text/plain", "ok");
}

static void handleWifiRemove() {
  gSettings.removeNetwork(server.arg("ssid"));
  gSettings.save();
  server.send(200, "text/plain", "ok");
}

static void handleTime() {
  if (server.hasArg("epoch")) net::setTimeFromEpoch((uint32_t)server.arg("epoch").toInt(), "browser");
  server.send(200, "text/plain", "ok");
}

// ---- OTA firmware update (browser upload of a .bin) ----
static const char UPDATE_HTML[] PROGMEM = R"HTML(<!doctype html><meta name=viewport content="width=device-width,initial-scale=1">
<title>Huckleberry firmware</title><body style="font-family:system-ui;background:#0b0805;color:#cbb38c;padding:24px">
<h2 style="color:#F4791F">Firmware update</h2>
<p>Current: <b>v0.2.0-b1</b>. Upload <code>.pio/build/huckleberry/firmware.bin</code>.</p>
<form method=POST action=/update enctype=multipart/form-data>
<input type=file name=fw accept=".bin" style="color:#cbb38c"><br><br>
<input type=submit value="Flash &amp; reboot" style="background:#F4791F;color:#1a0f00;border:0;border-radius:8px;padding:10px 16px;font-size:16px">
</form><p style="color:#8a7c63">Or over Wi-Fi: <code>pio run -e huckleberry_ota -t upload</code></p></body>)HTML";

static void otaUpload() {
  HTTPUpload& up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    Update.begin(UPDATE_SIZE_UNKNOWN);
  } else if (up.status == UPLOAD_FILE_WRITE) {
    Update.write(up.buf, up.currentSize);
  } else if (up.status == UPLOAD_FILE_END) {
    Update.end(true);
  }
}

void begin() {
  server.on("/", HTTP_GET, [] { server.send_P(200, "text/html", PAGE); });
  server.on("/api/state", HTTP_GET, handleState);
  server.on("/api/settings", HTTP_POST, handleSettings);
  server.on("/api/wifi/add", HTTP_POST, handleWifiAdd);
  server.on("/api/wifi/remove", HTTP_POST, handleWifiRemove);
  server.on("/api/time", HTTP_POST, handleTime);
  server.on("/update", HTTP_GET, [] { server.send_P(200, "text/html", UPDATE_HTML); });
  server.on("/update", HTTP_POST, [] {
    bool ok = !Update.hasError();
    server.send(200, "text/html", ok ? "<h3>Update OK — rebooting…</h3>" : "<h3>Update FAILED</h3>");
    delay(600);
    ESP.restart();
  }, otaUpload);
  server.onNotFound([] {  // captive-portal friendliness in AP mode
    server.sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/");
    server.send(302, "text/plain", "");
  });
  server.begin();
}

void loop() { server.handleClient(); }

} // namespace web
