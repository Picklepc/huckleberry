#include "WebApp.h"
#include "Settings.h"
#include "AppState.h"
#include "Net.h"
#include "HuckTheme.h"

#include <WebServer.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include <Update.h>
#include <FS.h>
#include <SPIFFS.h>
#include <time.h>

namespace web {

#define FW_VERSION "v0.2.0"

static WebServer server(80);
static bool s_fsOk = false;
static File s_bgUpload;
static bool s_bgUploadOk = false;
static uint32_t s_bgRev = 1;

static int clampWebInt(int v, int lo, int hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

static bool isSafeBgName(const String& name) {
  if (name.length() < 5 || name.length() > 48) return false;
  String lower = name;
  lower.toLowerCase();
  if (!lower.endsWith(".jpg") && !lower.endsWith(".jpeg")) return false;
  for (size_t i = 0; i < name.length(); i++) {
    char c = name[i];
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
    if (!ok) return false;
  }
  return true;
}

static String bgPath(const String& name) { return String("/bg/") + name; }

static void addBackgrounds(JsonArray arr) {
  if (!s_fsOk) return;
  File root = SPIFFS.open("/bg");
  if (!root || !root.isDirectory()) root = SPIFFS.open("/");
  if (!root) return;
  File f = root.openNextFile();
  while (f) {
    String name = f.name();
    int slash = name.lastIndexOf('/');
    if (slash >= 0) name = name.substring(slash + 1);
    if (!f.isDirectory() && isSafeBgName(name)) arr.add(name);
    f = root.openNextFile();
  }
}

// ---------------------------------------------------------------- page (SPA)
static const char PAGE[] PROGMEM = R"HTML(<!doctype html><html><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Huckleberry</title><style>
:root{--bg:#0b0805;--panel:#171008;--acc:#F4791F;--acc2:#8f7bd6;--tx:#cbb38c;--hi:#ffe9c8}
*{box-sizing:border-box}body{margin:0;font-family:system-ui,-apple-system,sans-serif;background:var(--bg);color:var(--tx)}
header{padding:16px 18px;display:flex;align-items:center;gap:16px}
header b{color:var(--acc);letter-spacing:3px;font-size:18px}
.flower{width:20px;height:20px;flex:0 0 20px;border-radius:50%;background:var(--acc2);box-shadow:0 -11px var(--acc),0 11px var(--acc),-11px 0 var(--acc),11px 0 var(--acc)}
.wrap{max-width:680px;margin:0 auto;padding:0 14px 40px}
.card{background:var(--panel);border-radius:16px;padding:16px 18px;margin:12px 0;box-shadow:0 4px 18px #0007}
.card h2{margin:0 0 12px;font-size:13px;letter-spacing:2px;color:var(--acc);text-transform:uppercase}
.big{font-size:44px;color:var(--hi);font-weight:700;line-height:1}
.row{display:flex;justify-content:space-between;gap:10px;margin:6px 0}
.k{color:#8a7c63}.v{color:var(--hi)}
.grid{display:grid;grid-template-columns:1fr 1fr;gap:10px}
.metric{display:grid;grid-template-columns:auto 1fr;gap:3px 10px;align-items:baseline}
.metric .val{font-size:32px;color:var(--hi);font-weight:700;line-height:1}
.metric .unit{color:var(--tx)}
.cells{display:grid;grid-template-columns:repeat(auto-fit,minmax(64px,1fr));gap:8px;margin-top:8px}
.cell{background:#0f0a04;border:1px solid #3a2a12;border-radius:8px;padding:8px;text-align:center}
button{background:#241a0d;color:var(--hi);border:1px solid #3a2a12;border-radius:10px;padding:10px 14px;font-size:16px;cursor:pointer}
button.acc{background:var(--acc);color:#1a0f00;border:0}
input,select{width:100%;padding:9px;border-radius:9px;border:1px solid #3a2a12;background:#0f0a04;color:var(--hi)}
label{display:block;font-size:12px;color:#8a7c63;margin:8px 0 3px}
.pill{display:inline-block;padding:3px 9px;border-radius:20px;background:#241a0d;font-size:12px}
.preview{display:block;width:100%;max-width:320px;aspect-ratio:4/3;object-fit:cover;background:#050505;border:1px solid #3a2a12;border-radius:8px;margin-top:8px}
.micro{font-size:12px;color:#8a7c63}
.spin{margin-left:6px;font-size:44px;color:var(--hi)}
.thermo{display:flex;align-items:center;justify-content:center;gap:14px}
details summary{cursor:pointer;color:var(--acc);letter-spacing:2px;font-size:13px;text-transform:uppercase}
.ok{color:#7db56a}.bad{color:#e0725a}
</style></head><body>
<header><span class="flower"></span><b>HUCKLEBERRY</b><span id="clk" style="margin-left:auto;color:var(--hi);font-variant-numeric:tabular-nums"></span></header>
<div class="wrap">
 <div class="card"><h2>Battery / Solar</h2>
  <div class="grid">
   <div class="metric"><span class="val"><span id="soc">--</span>%</span><span class="unit">SOC</span>
    <span class="val" id="btp">--</span><span class="unit">battery W</span></div>
   <div class="metric"><span class="val"><span id="pv">--</span>W</span><span class="unit">solar</span>
    <span class="val" id="ss">--</span><span class="unit">Victron</span></div>
  </div>
  <div class="grid" style="margin-top:12px">
   <div>
    <div class="row"><span class="k">Battery status</span><span class="v" id="bst">--</span></div>
    <div class="row"><span class="k">Total voltage</span><span class="v" id="bv">--</span></div>
    <div class="row"><span class="k">Total current</span><span class="v" id="ba">--</span></div>
    <div class="row"><span class="k">Remaining capacity</span><span class="v" id="brc">--</span></div>
    <div class="row"><span class="k">Remaining time</span><span class="v" id="bwt">--</span></div>
    <div class="row"><span class="k">Temperature</span><span class="v" id="btm">--</span></div>
   </div>
   <div>
    <div class="row"><span class="k">Available capacity</span><span class="v" id="bnc">--</span></div>
    <div class="row"><span class="k">Cell count</span><span class="v" id="bct">--</span></div>
    <div class="row"><span class="k">Cycles</span><span class="v" id="bcy">--</span></div>
    <div class="row"><span class="k">Solar output</span><span class="v" id="spct">--</span></div>
    <div class="row"><span class="k">Solar peak</span><span class="v" id="smax">--</span></div>
    <div class="row"><span class="k">Solar battery</span><span class="v" id="sbatt">--</span></div>
    <div class="row"><span class="k">Yield today</span><span class="v" id="yd">--</span></div>
    <div class="row"><span class="k">Solar RSSI</span><span class="v" id="srssi">--</span></div>
   </div>
  </div>
  <div class="k" style="margin-top:10px">Single cell voltage</div>
  <div id="bcells" class="cells"></div>
 </div>
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
  <div class="row"><span class="k">Firmware</span><span class="v" id="fw">--</span></div></div>
 <div class="card"><details><summary>Settings</summary>
  <label>Saved Wi-Fi networks (home + campsites)</label>
  <div id="netlist" class="k">none yet</div>
  <div class="grid" style="margin-top:8px">
   <input id="fssid" placeholder="SSID"><input id="fpass" type="password" placeholder="password">
  </div>
  <button class="acc" style="margin-top:8px" onclick="addNet()">Add network</button>
  <p class="k">The trailer auto-joins the first saved network it finds; the <b>Huckleberry</b> hotspot stays on for off-grid.</p>
  <label style="margin-top:14px">Color scheme</label><select id="theme" onchange="save()"></select>
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
  <label style="margin-top:12px">Screen background</label>
  <div class="grid"><div><label>Screen</label><select id="bgPage" onchange="renderBackgrounds()"></select></div>
   <div><label>Background</label><select id="bgSel" onchange="backgroundChanged()"></select></div></div>
  <img id="bgPrev" class="preview" alt="">
  <form id="bgForm" onsubmit="uploadBg(event)" style="margin-top:8px">
   <input id="bgFile" type="file" accept="image/jpeg">
   <button class="acc" style="margin-top:8px" type="submit">Upload JPEG</button>
  </form>
  <label style="margin-top:12px">Page display</label>
  <div class="grid"><div><label>Page</label><select id="ctlPage" onchange="paintPageControls()"></select></div>
   <div><label>Page theme</label><select id="pageTheme" onchange="pageControlChanged()"></select></div></div>
  <label style="margin-top:8px"><input type="checkbox" id="pageBox" style="width:auto" onchange="pageControlChanged()"> Data box</label>
  <div class="grid"><div><label>Scale</label><input id="layS" type="range" min="70" max="150" onchange="pageControlChanged()"></div>
   <div><label>X</label><input id="layX" type="number" min="-220" max="240" onchange="pageControlChanged()"></div></div>
  <div class="grid"><div><label>Y</label><input id="layY" type="number" min="-120" max="192" onchange="pageControlChanged()"></div>
   <div><label>&nbsp;</label><button type="button" onclick="resetPageControls()">Reset page</button></div></div>
  <div class="micro">Night clock layout is fixed.</div>
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
const PAGE_NAMES=['Clock','Climate','Power','Status'];
const PAGE_LAYOUT_SLOT=[0,2,3,5];
const PAGE_LAYOUT_DEFAULT=[{x:14,y:30,scale:100},{x:12,y:72,scale:100},{x:12,y:70,scale:100},{x:164,y:46,scale:100}];
function $(i){return document.getElementById(i)}
function fmt(x,d,u){return (x==null||isNaN(x))?'--':(x.toFixed(d)+(u||''))}
function fmtTime(h){if(h==null||isNaN(h))return'--';let m=Math.round(h*60);return Math.floor(m/60)+'h '+String(m%60).padStart(2,'0')+'m'}
async function load(){try{S=await (await fetch('/api/state')).json();paint()}catch(e){}}
function paint(){
 $('soc').textContent=S.batt&&S.batt.soc>=0?S.batt.soc:'--';
 $('bv').textContent=fmt(S.batt&&S.batt.v,2,' V');$('ba').textContent=fmt(S.batt&&S.batt.a,2,' A');
 $('btp').textContent=fmt(S.batt&&S.batt.w,0,' W');$('bst').textContent=S.batt&&S.batt.status?S.batt.status:'--';
 $('brc').textContent=fmt(S.batt&&S.batt.resid,2,' Ah');$('bnc').textContent=fmt(S.batt&&S.batt.nom,0,' Ah');
 $('bwt').textContent=fmtTime(S.batt&&S.batt.workH);$('bcy').textContent=S.batt&&S.batt.cycles>=0?S.batt.cycles:'--';
 $('bct').textContent=S.batt&&S.batt.cellCount>0?S.batt.cellCount:'--';
 let temps=(S.batt&&S.batt.temps)||[];$('btm').textContent=temps.length?temps.map(t=>fmt(t,1,'°F')).join(' / '):'--';
 let cells=(S.batt&&S.batt.cellMv)||[];$('bcells').innerHTML=cells.length?cells.map((mv,i)=>`<div class=cell><div class=k>${String(i+1).padStart(2,'0')}</div><div class=v>${(mv/1000).toFixed(3)} V</div></div>`).join(''):'<div class=k>waiting</div>';
 $('pv').textContent=S.sol&&S.sol.valid?Math.round(S.sol.pv):'--';
  $('ss').textContent=S.sol?S.sol.state:'--';$('yd').textContent=fmt(S.sol&&S.sol.yield,2,' kWh');
 $('spct').textContent=S.sol&&S.sol.valid&&S.sol.pvPct!=null?(Math.round(S.sol.pvPct)+'% of max'):'--';
 $('smax').textContent=fmt(S.sol&&S.sol.pvMax,0,' W');
  $('sbatt').textContent=S.sol&&S.sol.valid?(fmt(S.sol.v,2,' V')+' / '+fmt(S.sol.a,1,' A')):'--';
  $('srssi').textContent=S.sol&&S.sol.valid?(S.sol.rssi+' dBm'):'--';
 $('set').textContent=S.th.sp;$('mode').value=S.th.mode;$('camp').value=S.th.camp?1:0;
 $('wifi').innerHTML=S.net.sta?'<span class=ok>connected</span>':'<span class=bad>offline</span>';
 $('ssid').textContent=S.net.ssid||'--';$('ip').textContent=S.net.ip||'--';
 $('ap').textContent=S.net.ap||'--';$('tsrc').textContent=S.net.tsrc;
 $('fw').textContent=S.fw||'--';
 $('blink').innerHTML=S.batt&&S.batt.valid?'<span class=ok>live</span>':'waiting…';
 $('slink').innerHTML=S.sol&&S.sol.valid?'<span class=ok>live ('+S.sol.rssi+'dBm)</span>':'waiting…';
 if(!themeInit){
   $('theme').innerHTML=S.themes.map((n,i)=>`<option value=${i}>${n}</option>`).join('');
   $('bgPage').innerHTML=PAGE_NAMES.map((n,i)=>`<option value=${i}>${n}</option>`).join('');
   $('ctlPage').innerHTML=PAGE_NAMES.map((n,i)=>`<option value=${i}>${n}</option>`).join('');
   $('pageTheme').innerHTML='<option value="-1">Color scheme</option>'+S.themes.map((n,i)=>`<option value=${i}>${n}</option>`).join('');
   let hrs='';for(let h=0;h<24;h++){let ap=h<12?'AM':'PM',hh=h%12||12;hrs+=`<option value=${h}>${hh} ${ap}</option>`}
   ['nStart','nEnd','doStart','doEnd'].forEach(id=>$(id).innerHTML=hrs);
   themeInit=1}
 let s=S.set;
 $('theme').value=s.theme;$('bright').value=s.bright;$('anim').checked=s.anim;
 $('nbright').value=s.nbright;$('autoNight').checked=s.autoNight;
 $('nStart').value=s.nStart;$('nEnd').value=s.nEnd;$('hto').value=s.hto;
 $('doEn').checked=s.doEn;$('doStart').value=s.doStart;$('doEnd').value=s.doEnd;
 $('stMin').value=s.stMin;$('stMax').value=s.stMax;
 renderBackgrounds();paintPageControls();
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
function renderBackgrounds(){
 let s=S.set||{}, bgs=S.bgs||[], page=+$('bgPage').value||0;
 let cur=(s.pageBg&&s.pageBg.length)?(s.pageBg[page]||''):(s.dayBg||'');
  let opts=bgs.slice();
  if(cur&&!opts.includes(cur))opts.unshift(cur);
  if(!opts.length)opts=[cur||'none'];
  $('bgSel').innerHTML=opts.map(n=>`<option value="${n==='none'?'':n}">${n}</option>`).join('');
  $('bgSel').value=cur;
  $('bgPrev').style.display=cur?'block':'none';
  if(cur)$('bgPrev').src='/bg/'+encodeURIComponent(cur)+'?v='+(S.bgRev||0);
}
function backgroundChanged(){
 let p=+$('bgPage').value||0;
 if(!S.set)S.set={};
 if(!S.set.pageBg)S.set.pageBg=['','','',''];
 S.set.pageBg[p]=$('bgSel').value;
 save();
}
function layoutSlot(i){
 if(!S.set)S.set={};
 if(!S.set.layout)S.set.layout=[];
 if(!S.set.layout[i])S.set.layout[i]={x:0,y:0,scale:100};
 return S.set.layout[i];
}
function pageIndex(){return +$('ctlPage').value||0}
function pageLayoutSlot(p){return layoutSlot(PAGE_LAYOUT_SLOT[p])}
function paintPageControls(){
 let p=pageIndex(),l=pageLayoutSlot(p),s=S.set||{};
 $('layX').value=l.x;$('layY').value=l.y;$('layS').value=l.scale;
 $('pageTheme').value=(s.pageTheme&&s.pageTheme[p]!=null)?s.pageTheme[p]:-1;
 $('pageBox').checked=!!(s.pageBox&&s.pageBox[p]);
}
function pageControlChanged(){
 let p=pageIndex(),l=pageLayoutSlot(p),s=S.set||{};
 l.x=+$('layX').value||0;l.y=+$('layY').value||0;l.scale=+$('layS').value||100;
 if(!s.pageTheme)s.pageTheme=[-1,-1,-1,-1];s.pageTheme[p]=+$('pageTheme').value;
 if(!s.pageBox)s.pageBox=[0,0,0,0];s.pageBox[p]=$('pageBox').checked?1:0;S.set=s;
 save();
}
function resetPageControls(){
 let p=pageIndex(),d=PAGE_LAYOUT_DEFAULT[p],l=pageLayoutSlot(p);
 l.x=d.x;l.y=d.y;l.scale=d.scale;
 paintPageControls();
  save();
}
function post(u,b){return fetch(u,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:b})}
function sp(d){S.th.sp=Math.max(45,Math.min(90,(S.th.sp|0)+d));$('set').textContent=S.th.sp;save()}
function save(){let body=`sp=${$('set').textContent}&mode=${$('mode').value}&camp=${$('camp').value}&theme=${$('theme').value}&bright=${$('bright').value}&anim=${$('anim').checked?1:0}`+
 `&nbright=${$('nbright').value}&autoNight=${$('autoNight').checked?1:0}&nStart=${$('nStart').value}&nEnd=${$('nEnd').value}&hto=${$('hto').value}`+
 `&doEn=${$('doEn').checked?1:0}&doStart=${$('doStart').value}&doEnd=${$('doEnd').value}&stMin=${$('stMin').value}&stMax=${$('stMax').value}`;
 let pbg=(S.set&&S.set.pageBg)||[];
 for(let i=0;i<PAGE_NAMES.length;i++)body+=`&bg${i}=${encodeURIComponent(pbg[i]||'')}`;
 let pt=(S.set&&S.set.pageTheme)||[],pb=(S.set&&S.set.pageBox)||[];
 for(let i=0;i<PAGE_NAMES.length;i++)body+=`&pt${i}=${pt[i]??-1}&pb${i}=${pb[i]?1:0}`;
 for(let i=0;i<PAGE_LAYOUT_SLOT.length;i++){let l=pageLayoutSlot(i),slot=PAGE_LAYOUT_SLOT[i];body+=`&lx${slot}=${l.x}&ly${slot}=${l.y}&ls${slot}=${l.scale}`}
 post('/api/settings',body)}
function uploadBg(e){e.preventDefault();let f=$('bgFile').files[0];if(!f)return;let fd=new FormData();fd.append('bg',f,'custom.jpg');fetch('/api/background/upload?page='+($('bgPage').value||0),{method:'POST',body:fd}).then(()=>{$('bgFile').value='';setTimeout(load,600)})}
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

static const char* battStatusName(const Telemetry& t) {
  if (!t.battValid || isnan(t.battAmps)) return "waiting";
  if (t.battAmps > 0.05f) return "Charging";
  if (t.battAmps < -0.05f) return "Discharging";
  return "Idle";
}

static float workHoursRemaining(const Telemetry& t) {
  if (!t.battValid || isnan(t.battResidAh) || isnan(t.battAmps) || t.battAmps >= -0.05f) return NAN;
  return t.battResidAh / fabsf(t.battAmps);
}

static LayoutSlot clampLayoutForSlot(LayoutWidget w, LayoutSlot slot) {
  static const uint16_t dims[LAYOUT_WIDGET_COUNT][2] = {
    {252, 130}, {230, 24}, {158, 132}, {294, 138}, {146, 132}, {154, 152}
  };
  static const uint8_t minScale[LAYOUT_WIDGET_COUNT] = {70, 70, 75, 75, 75, 75};
  static const uint8_t maxScale[LAYOUT_WIDGET_COUNT] = {135, 140, 150, 125, 150, 150};
  int idx = (int)w;
  slot.scale = clampWebInt(slot.scale, minScale[idx], maxScale[idx]);
  int scaledW = ((int)dims[idx][0] * slot.scale + 99) / 100;
  int scaledH = ((int)dims[idx][1] * slot.scale + 99) / 100;
  int minX = min(0, 320 - scaledW);
  int maxX = max(0, 320 - scaledW);
  int minY = min(0, 240 - scaledH);
  int maxY = max(0, 240 - scaledH);
  slot.x = clampWebInt(slot.x, minX, maxX);
  slot.y = clampWebInt(slot.y, minY, maxY);
  return slot;
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
  if (!isnan(t.battPowerW)) b["w"] = t.battPowerW;
  if (!isnan(t.battResidAh)) b["resid"] = t.battResidAh;
  if (!isnan(t.battNomAh)) b["nom"] = t.battNomAh;
  if (!isnan(workHoursRemaining(t))) b["workH"] = workHoursRemaining(t);
  b["cycles"] = t.battCycles;
  b["cellCount"] = t.battCellCount;
  b["tempCount"] = t.battTempCount;
  b["status"] = battStatusName(t);
  b["fet"] = t.battFet;
  b["protect"] = t.battProtect;
  b["sw"] = t.battSw;
  auto temps = b["temps"].to<JsonArray>();
  for (int i = 0; i < t.battTempCount && i < (int)HUCK_MAX_BATT_TEMPS; i++) {
    if (!isnan(t.battTempsC[i])) temps.add(t.battTempsC[i] * 9.0f / 5.0f + 32.0f);
  }
  auto cells = b["cellMv"].to<JsonArray>();
  int cellCount = t.battCellCount;
  if (cellCount < 0 || cellCount > (int)HUCK_MAX_BATT_CELLS) cellCount = HUCK_MAX_BATT_CELLS;
  for (int i = 0; i < cellCount; i++) {
    if (t.battCellMv[i] > 0) cells.add(t.battCellMv[i]);
  }
  auto s = d["sol"].to<JsonObject>();
  s["valid"] = t.solValid; s["pv"] = isnan(t.solPvW) ? 0.0f : t.solPvW;
  s["state"] = solStateName(t.solState);
  if (!isnan(t.solPvMaxW)) s["pvMax"] = t.solPvMaxW;
  if (!isnan(t.solPvW) && !isnan(t.solPvMaxW) && t.solPvMaxW > 0.5f) {
    s["pvPct"] = t.solPvW * 100.0f / t.solPvMaxW;
  }
  if (!isnan(t.solBattV)) s["v"] = t.solBattV;
  if (!isnan(t.solBattA)) s["a"] = t.solBattA;
  if (!isnan(t.solLoadA)) s["loadA"] = t.solLoadA;
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
  st["dayBg"] = gSettings.pageBg[PAGE_CLOCK];
  auto pageBg = st["pageBg"].to<JsonArray>();
  for (size_t i = 0; i < gSettings.pageBg.size(); i++) pageBg.add(gSettings.pageBg[i]);
  auto pageTheme = st["pageTheme"].to<JsonArray>();
  for (size_t i = 0; i < gSettings.pageTheme.size(); i++) pageTheme.add(gSettings.pageTheme[i]);
  auto pageBox = st["pageBox"].to<JsonArray>();
  for (size_t i = 0; i < gSettings.pageBox.size(); i++) pageBox.add(gSettings.pageBox[i] ? 1 : 0);
  auto layout = st["layout"].to<JsonArray>();
  for (size_t i = 0; i < gSettings.layout.size(); i++) {
    JsonObject o = layout.add<JsonObject>();
    o["x"] = gSettings.layout[i].x;
    o["y"] = gSettings.layout[i].y;
    o["scale"] = gSettings.layout[i].scale;
  }
  auto themes = d["themes"].to<JsonArray>();
  for (size_t i = 0; i < HUCK_THEME_COUNT; i++) themes.add(HUCK_THEMES[i].name);
  auto bgs = d["bgs"].to<JsonArray>();
  addBackgrounds(bgs);
  d["bgRev"] = s_bgRev;
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
  auto setBg = [](size_t idx, const String& bg) {
    if (idx >= gSettings.pageBg.size()) return;
    if (bg.isEmpty() || (isSafeBgName(bg) && (!s_fsOk || SPIFFS.exists(bgPath(bg))))) {
      if (gSettings.pageBg[idx] != bg) gBgReloadRequested = true;
      gSettings.pageBg[idx] = bg;
    }
  };
  if (server.hasArg("bg")) setBg(PAGE_CLOCK, server.arg("bg"));
  for (size_t i = 0; i < gSettings.pageBg.size(); i++) {
    char kb[8];
    snprintf(kb, sizeof(kb), "bg%u", (unsigned)i);
    if (server.hasArg(kb)) setBg(i, server.arg(kb));
    char kt[8], kbox[8];
    snprintf(kt, sizeof(kt), "pt%u", (unsigned)i);
    snprintf(kbox, sizeof(kbox), "pb%u", (unsigned)i);
    if (server.hasArg(kt)) {
      gSettings.pageTheme[i] = (int8_t)clampWebInt(server.arg(kt).toInt(), -1, (int)HUCK_THEME_COUNT - 1);
    }
    if (server.hasArg(kbox)) gSettings.pageBox[i] = server.arg(kbox).toInt() != 0;
  }
  for (size_t i = 0; i < gSettings.layout.size(); i++) {
    char kx[8], ky[8], ks[8];
    snprintf(kx, sizeof(kx), "lx%u", (unsigned)i);
    snprintf(ky, sizeof(ky), "ly%u", (unsigned)i);
    snprintf(ks, sizeof(ks), "ls%u", (unsigned)i);
    if (server.hasArg(kx)) gSettings.layout[i].x = server.arg(kx).toInt();
    if (server.hasArg(ky)) gSettings.layout[i].y = server.arg(ky).toInt();
    if (server.hasArg(ks)) gSettings.layout[i].scale = server.arg(ks).toInt();
    gSettings.layout[i] = clampLayoutForSlot((LayoutWidget)i, gSettings.layout[i]);
  }
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

static void handleBgFile() {
  if (!s_fsOk) { server.send(404, "text/plain", "no filesystem"); return; }
  String uri = server.uri();
  String name = uri.startsWith("/bg/") ? uri.substring(4) : "";
  if (!isSafeBgName(name)) { server.send(404, "text/plain", "bad background"); return; }
  File f = SPIFFS.open(bgPath(name), "r");
  if (!f) { server.send(404, "text/plain", "not found"); return; }
  server.streamFile(f, "image/jpeg");
  f.close();
}

static void backgroundUpload() {
  HTTPUpload& up = server.upload();
  if (!s_fsOk) return;
  if (up.status == UPLOAD_FILE_START) {
    s_bgUploadOk = true;
    if (s_bgUpload) s_bgUpload.close();
    SPIFFS.mkdir("/bg");
    s_bgUpload = SPIFFS.open("/bg/custom.jpg", "w");
    if (!s_bgUpload) s_bgUploadOk = false;
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (s_bgUpload && s_bgUpload.write(up.buf, up.currentSize) != up.currentSize) s_bgUploadOk = false;
  } else if (up.status == UPLOAD_FILE_END) {
    if (s_bgUpload) s_bgUpload.close();
    if (s_bgUploadOk) {
      s_bgRev++;
    }
  }
}

// ---- OTA firmware update (browser upload of a .bin) ----
static const char UPDATE_HTML[] PROGMEM = R"HTML(<!doctype html><meta name=viewport content="width=device-width,initial-scale=1">
<title>Huckleberry firmware</title><body style="font-family:system-ui;background:#0b0805;color:#cbb38c;padding:24px">
<h2 style="color:#F4791F">Firmware update</h2>
<p>Current: <b>v0.2.0</b>. Upload <code>.pio/build/huckleberry/firmware.bin</code>.</p>
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
  s_fsOk = SPIFFS.begin(false);
  if (!s_fsOk) s_fsOk = SPIFFS.begin(true);
  if (s_fsOk) SPIFFS.mkdir("/bg");

  server.on("/", HTTP_GET, [] { server.send_P(200, "text/html", PAGE); });
  server.on("/api/state", HTTP_GET, handleState);
  server.on("/api/settings", HTTP_POST, handleSettings);
  server.on("/api/wifi/add", HTTP_POST, handleWifiAdd);
  server.on("/api/wifi/remove", HTTP_POST, handleWifiRemove);
  server.on("/api/time", HTTP_POST, handleTime);
  server.on("/api/background/upload", HTTP_POST, [] {
    bool ok = s_fsOk && s_bgUploadOk && SPIFFS.exists("/bg/custom.jpg");
    if (ok) {
      int page = server.hasArg("page") ? clampWebInt(server.arg("page").toInt(), 0, PAGE_COUNT - 1) : PAGE_CLOCK;
      gSettings.pageBg[page] = "custom.jpg";
      gSettings.save();
      gUiApplyRequested = true;
      gBgReloadRequested = true;
    }
    server.send(ok ? 200 : 500, "text/plain", ok ? "ok" : "upload failed");
  }, backgroundUpload);
  server.on("/update", HTTP_GET, [] { server.send_P(200, "text/html", UPDATE_HTML); });
  server.on("/update", HTTP_POST, [] {
    bool ok = !Update.hasError();
    server.send(200, "text/html", ok ? "<h3>Update OK - rebooting...</h3>" : "<h3>Update FAILED</h3>");
    delay(600);
    ESP.restart();
  }, otaUpload);
  server.onNotFound([] {  // captive-portal friendliness in AP mode
    if (server.uri().startsWith("/bg/")) { handleBgFile(); return; }
    server.sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/");
    server.send(302, "text/plain", "");
  });
  server.begin();
}

void loop() { server.handleClient(); }

} // namespace web
