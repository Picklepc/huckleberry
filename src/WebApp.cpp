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

#define FW_VERSION "v0.4.0"

static WebServer server(80);
static bool s_fsOk = false;
static File s_bgUpload;
static bool s_bgUploadOk = false;
static uint32_t s_bgRev = 1;
// Which layout slot each display page owns. Must match PAGE_LAYOUT_SLOT in the SPA.
static const uint8_t PAGE_LAYOUT_SLOT[PAGE_COUNT] = {
  LAYOUT_DAY_CLOCK, LAYOUT_THERMO_CARD, LAYOUT_POWER_GAUGE, LAYOUT_STATUS_CARD
};

// Compiled-in defaults used by /api/reset — kept in sync with Settings.h.
static const char* PAGE_BG_DEFAULT[PAGE_COUNT] = {
  "bg_indie_02.jpg", "bg_charlie_01.jpg", "bg_creek_01.jpg", "bg_indie_01.jpg"
};
static const int8_t PAGE_THEME_DEFAULT[PAGE_COUNT] = { -1, -1, -1, -1 };
static const bool PAGE_BOX_DEFAULT[PAGE_COUNT] = { false, true, true, true };
static const uint8_t PAGE_CONTRAST_DEFAULT[PAGE_COUNT] = { 0, 0, 0, 0 };
static const LayoutSlot LAYOUT_DEFAULT[LAYOUT_WIDGET_COUNT] = {
  {14, 30, 100}, {12, 12, 100}, {12, 72, 100}, {12, 70, 100}, {160, 70, 100}, {164, 46, 100}
};

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

static bool isSafePresetName(const String& n) {
  if (n.length() < 1 || n.length() > 32) return false;
  for (size_t i = 0; i < n.length(); i++) {
    char c = n[i];
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == ' ' || c == '_' || c == '-';
    if (!ok) return false;
  }
  return true;
}

static bool isSafeHostname(const String& n) {
  if (n.length() < 1 || n.length() > 24) return false;
  for (size_t i = 0; i < n.length(); i++) {
    char c = n[i];
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-';
    if (!ok) return false;
  }
  // no leading/trailing hyphen (RFC 952-ish)
  if (n[0] == '-' || n[n.length() - 1] == '-') return false;
  return true;
}

static bool isSafeAccent(const String& v) {
  if (v.isEmpty()) return true;   // empty = reset to default
  if (v.length() != 7 || v[0] != '#') return false;
  for (size_t i = 1; i < 7; i++) {
    char c = v[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
  }
  return true;
}

static bool isVictronPin(const String& value) {
  if (value.length() != 6) return false;
  for (size_t i = 0; i < value.length(); i++) {
    if (value[i] < '0' || value[i] > '9') return false;
  }
  return true;
}

static String bgPath(const String& name) { return String("/bg/") + name; }
static String presetPath(const String& name) { return String("/presets/") + name + ".json"; }

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

static void listPresets(JsonArray arr) {
  if (!s_fsOk) return;
  File d = SPIFFS.open("/presets");
  if (!d || !d.isDirectory()) return;
  File f = d.openNextFile();
  while (f) {
    if (!f.isDirectory()) {
      String name = f.name();
      int slash = name.lastIndexOf('/'); if (slash >= 0) name = name.substring(slash + 1);
      if (name.endsWith(".json")) arr.add(name.substring(0, name.length() - 5));
    }
    f = d.openNextFile();
  }
}

// ---------------------------------------------------------------- page (SPA)
static const char PAGE[] PROGMEM = R"HTML(<!doctype html><html><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Huckleberry</title><style>
:root{--bg:#0b0805;--panel:#171008;--acc:#F4791F;--acc2:#8f7bd6;--tx:#cbb38c;--hi:#ffe9c8;--ok:#7db56a;--bad:#e0725a}
*{box-sizing:border-box}html,body{margin:0;background:var(--bg);color:var(--tx);font-family:system-ui,-apple-system,sans-serif}
header{padding:12px 16px;display:flex;align-items:center;gap:12px;border-bottom:1px solid #241a0d;position:sticky;top:0;background:var(--bg);z-index:10}
header b{color:var(--acc);letter-spacing:3px;font-size:16px}
.flower{width:20px;height:20px;flex:0 0 20px;border-radius:50%;background:var(--acc2);box-shadow:0 -11px var(--acc),0 11px var(--acc),-11px 0 var(--acc),11px 0 var(--acc)}
#clk{margin-left:auto;color:var(--hi);font-variant-numeric:tabular-nums;font-size:14px}
nav{display:flex;overflow-x:auto;background:#0d0703;border-bottom:1px solid #241a0d;position:sticky;top:45px;z-index:9}
nav button{background:transparent;color:var(--tx);border:0;border-bottom:2px solid transparent;padding:12px 14px;font-size:12px;letter-spacing:1px;cursor:pointer;white-space:nowrap;text-transform:uppercase}
nav button.on{color:var(--hi);border-bottom-color:var(--acc)}
.wrap{max-width:960px;margin:0 auto;padding:12px 14px 60px}
section:not(#power){max-width:680px;margin:0 auto}
.card{background:var(--panel);border-radius:14px;padding:14px 16px;margin:10px 0;box-shadow:0 4px 18px #0007}
.card h2{margin:0 0 10px;font-size:12px;letter-spacing:2px;color:var(--acc);text-transform:uppercase}
.big{font-size:42px;color:var(--hi);font-weight:700;line-height:1}
.row{display:flex;justify-content:space-between;gap:10px;margin:5px 0}
.k{color:#8a7c63}.v{color:var(--hi)}
.grid{display:grid;grid-template-columns:1fr 1fr;gap:10px}
.metric{display:grid;grid-template-columns:auto 1fr;gap:3px 10px;align-items:baseline}
.metric .val{font-size:28px;color:var(--hi);font-weight:700;line-height:1}
.metric .unit{color:var(--tx)}
.cells{display:grid;grid-template-columns:repeat(auto-fit,minmax(64px,1fr));gap:8px;margin-top:8px}
.cell{background:#0f0a04;border:1px solid #3a2a12;border-radius:8px;padding:6px;text-align:center;font-size:11px}
button{background:#241a0d;color:var(--hi);border:1px solid #3a2a12;border-radius:10px;padding:10px 14px;font-size:14px;cursor:pointer}
button.acc{background:var(--acc);color:#1a0f00;border:0}
button.danger{background:#3a1010;color:#ffbaba;border:1px solid #6a1a1a}
input,select{width:100%;padding:8px;border-radius:9px;border:1px solid #3a2a12;background:#0f0a04;color:var(--hi);font-size:14px}
input[type=color]{padding:2px;height:38px}
label{display:block;font-size:11px;color:#8a7c63;margin:8px 0 3px}
.pill{display:inline-block;padding:2px 8px;border-radius:20px;background:#241a0d;font-size:11px;text-align:center}
.ok{color:var(--ok)}.bad{color:var(--bad)}
.micro{font-size:11px;color:#8a7c63}
section{display:none}
section.on{display:block}
.thermo{display:flex;align-items:center;justify-content:center;gap:14px}
.preview{position:relative;width:100%;max-width:320px;aspect-ratio:4/3;background:#050505;border:1px solid #3a2a12;border-radius:8px;margin-top:8px;overflow:hidden}
.preview img{position:absolute;inset:0;width:100%;height:100%;object-fit:cover}
.preview .box{position:absolute;background:rgba(20,10,4,0.75);border:1px solid var(--acc);border-radius:8px;color:var(--hi);font-size:10px;padding:4px 6px;display:flex;align-items:center;justify-content:center;text-align:center;line-height:1.1;box-sizing:border-box;cursor:move;touch-action:none;user-select:none}
.preview .box.dragging{border-width:2px;box-shadow:0 0 0 3px rgba(244,121,31,0.35)}
.preview .box.noBox{background:transparent;border-color:transparent;text-shadow:0 1px 3px #000,0 0 4px #000}
.rowlist>div{display:flex;justify-content:space-between;align-items:center;gap:8px;padding:6px 0;border-bottom:1px solid #241a0d}
.rowlist>div:last-child{border-bottom:0}
.chart{display:block;width:100%;height:220px;background:#0f0a04;border:1px solid #3a2a12;border-radius:10px;margin-top:10px}
.chart.vh-overview{height:260px;cursor:pointer;touch-action:manipulation}
.chart.compact{height:176px;margin-top:0}
.cwrap{overflow-x:auto;-webkit-overflow-scrolling:touch;margin-top:7px}
.vh-head{display:flex;justify-content:space-between;align-items:start;gap:16px}
.vh-head .micro:last-child{text-align:right}
.vh-legend{display:flex;flex-wrap:wrap;gap:12px;margin:9px 2px 0;font-size:11px;color:#8a7c63}
.vh-legend span{display:inline-flex;align-items:center;gap:5px}
.vh-swatch{width:10px;height:10px;border-radius:2px;display:inline-block}
.vh-daytitle{margin:13px 0 7px;color:var(--hi);font-size:14px;font-weight:650}
.vh-daystats{display:grid;grid-template-columns:repeat(auto-fit,minmax(132px,1fr));gap:7px}
.vh-stat{background:#0f0a04;border:1px solid #2d2113;border-radius:8px;padding:7px 9px;min-width:0}
.vh-stat span{display:block;color:#8a7c63;font-size:10px;text-transform:uppercase;letter-spacing:.5px}
.vh-stat b{display:block;color:var(--hi);font-size:13px;margin-top:2px;overflow-wrap:anywhere}
.chartgrid{display:grid;grid-template-columns:repeat(auto-fit,minmax(280px,1fr));gap:10px;margin-top:12px}
.chartpanel{background:#120c05;border:1px solid #2d2113;border-radius:10px;padding:10px}
.chartpanel h3{margin:0;color:var(--hi);font-size:12px;font-weight:650}
.vh-errors{margin-top:10px;padding:9px 10px;background:#0f0a04;border:1px solid #2d2113;border-radius:8px}
code{background:#0f0a04;padding:1px 5px;border-radius:4px;color:var(--hi);font-size:11px}
.gauges{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:12px;margin-top:2px}
@media(max-width:640px){.gauges{grid-template-columns:repeat(2,minmax(0,1fr));gap:16px}}
.gaugePanel{display:grid;gap:7px;justify-items:center}
.gaugeDial{--pct:0;--accent:var(--acc);width:min(150px,100%);aspect-ratio:1;position:relative;border-radius:50%;background:conic-gradient(from 220deg,var(--accent) calc(var(--pct)*1deg),#2a1f12 0 320deg,#170f07 0);display:grid;place-items:center;transition:background .4s ease}
.gaugeDial::before{content:"";position:absolute;inset:13px;border-radius:50%;background:radial-gradient(circle at 50% 40%,#1d1409,#120c05);box-shadow:inset 0 0 0 1px #2a1f12}
.gaugeInner{position:relative;z-index:1;display:grid;gap:1px;justify-items:center;text-align:center;padding:6px}
.gaugeValue{font-size:1.7rem;font-weight:800;color:var(--hi);letter-spacing:-.03em;line-height:1}
.gaugeUnit{font-size:.62rem;letter-spacing:.13em;text-transform:uppercase;color:#8a7c63}
.gaugeLabel{font-size:.82rem;color:var(--tx);font-weight:600;letter-spacing:.02em}
.gaugeScale{width:78%;display:flex;justify-content:space-between;color:#6f6349;font-size:.64rem}
.vh-detailwrap{overflow-x:auto;-webkit-overflow-scrolling:touch;margin-top:8px}
.vh-detail{display:grid;grid-auto-rows:min-content;min-width:min-content}
.vh-detail .lbl{position:sticky;left:0;background:var(--panel);z-index:2;font-size:.66rem;color:#8a7c63;padding:3px 8px 3px 2px;text-align:right;white-space:nowrap;display:flex;align-items:center;justify-content:flex-end}
.vh-detail .grouphd{grid-column:1/-1;position:sticky;left:0;font-size:.6rem;letter-spacing:.12em;text-transform:uppercase;color:var(--acc);padding:7px 2px 3px;font-weight:650}
.vh-detail .hd{font-size:.62rem;color:#cbb38c;padding:4px 2px;text-align:center;white-space:nowrap;border-bottom:1px solid #2d2113}
.vh-detail .cell{font-size:.66rem;color:var(--hi);padding:3px 2px;text-align:center;font-variant-numeric:tabular-nums;white-space:nowrap;border-bottom:1px solid #1a1209}
.vh-detail .barcell{height:118px;display:flex;align-items:flex-end;justify-content:center;padding:0 6px 4px;cursor:pointer}
.vh-detail .barcell.sel .bar{outline:2px solid var(--hi);outline-offset:1px}
.vh-detail .hd{cursor:pointer}
.vh-detail .hd.sel{color:var(--acc);font-weight:700}
.vh-detail .bar{width:20px;min-height:2px;display:flex;flex-direction:column-reverse;border-radius:3px 3px 0 0;overflow:hidden;background:#241a0d}
.vh-detail .seg{width:100%}
</style></head><body>
<header><span class="flower"></span><b>HUCKLEBERRY</b><span id="clk"></span></header>
<nav>
  <button data-tab="dashboard">Dashboard</button>
  <button data-tab="power">Power</button>
  <button data-tab="display">Display</button>
  <button data-tab="net">Network</button>
  <button data-tab="ble">BLE</button>
  <button data-tab="fw">Firmware</button>
</nav>
<div class="wrap">
<section id="dashboard">
  <div class="card"><h2>Battery / Solar</h2>
    <div class="grid">
      <div class="metric"><span class="val"><span id="d-soc">--</span>%</span><span class="unit">SOC</span>
        <span class="val" id="d-btp">--</span><span class="unit">battery W</span></div>
      <div class="metric"><span class="val"><span id="d-pv">--</span>W</span><span class="unit">solar</span>
        <span class="val" id="d-ss">--</span><span class="unit">charger</span></div>
    </div>
  </div>
  <div class="card"><h2>Climate</h2>
    <div class="thermo"><button onclick="sp(-1)">&minus;</button>
      <div style="text-align:center"><div class="big"><span id="d-set">70</span>&deg;</div><div class="k">set point</div><div class="k" style="margin-top:4px">inside <span id="d-inside">--</span>&deg;</div></div>
      <button onclick="sp(1)">+</button></div>
    <div class="grid" style="margin-top:12px">
      <div><label>Mode</label><select id="d-mode" onchange="saveAll()">
        <option value=0>Auto</option><option value=1>Cool</option><option value=2>Heat</option><option value=3>Fan</option><option value=4>Off</option></select></div>
      <div><label>Preset</label><select id="d-camp" onchange="saveAll()"><option value=1>Camping</option><option value=0>Storing</option></select></div>
    </div>
    <div class="k" style="margin-top:8px">Gidrox 10k BTU &middot; <span id="d-gidrox">not yet paired</span></div>
  </div>
  <div class="card"><h2>Health</h2>
    <div class="row"><span class="k">Wi-Fi</span><span class="v" id="d-wifi">--</span></div>
    <div class="row"><span class="k">Battery link</span><span class="v" id="d-blink">--</span></div>
    <div class="row"><span class="k">Solar link</span><span class="v" id="d-slink">--</span></div>
    <div class="row"><span class="k">Firmware</span><span class="v" id="d-fw">--</span></div>
  </div>
</section>
<section id="power">
  <div class="card"><h2>Power flow</h2>
    <div class="gauges">
      <div class="gaugePanel">
        <div class="gaugeDial" id="g-soc" style="--accent:var(--acc)"><div class="gaugeInner"><div class="gaugeValue" id="gv-soc">--</div><div class="gaugeUnit">SOC %</div></div></div>
        <div class="gaugeLabel">Battery</div><div class="gaugeScale"><span>0</span><span>100</span></div>
      </div>
      <div class="gaugePanel">
        <div class="gaugeDial" id="g-pv" style="--accent:var(--ok)"><div class="gaugeInner"><div class="gaugeValue" id="gv-pv">--</div><div class="gaugeUnit">watts</div></div></div>
        <div class="gaugeLabel">Solar in</div><div class="gaugeScale"><span>0</span><span id="gs-pv">--</span></div>
      </div>
      <div class="gaugePanel">
        <div class="gaugeDial" id="g-bw" style="--accent:var(--acc2)"><div class="gaugeInner"><div class="gaugeValue" id="gv-bw">--</div><div class="gaugeUnit" id="gu-bw">watts</div></div></div>
        <div class="gaugeLabel" id="gl-bw">Battery flow</div><div class="gaugeScale"><span>use</span><span>charge</span></div>
      </div>
      <div class="gaugePanel">
        <div class="gaugeDial" id="g-load" style="--accent:var(--acc2)"><div class="gaugeInner"><div class="gaugeValue" id="gv-load">--</div><div class="gaugeUnit">amps</div></div></div>
        <div class="gaugeLabel">Load out</div><div class="gaugeScale"><span>0</span><span id="gs-load">15</span></div>
      </div>
    </div>
  </div>
  <div class="card"><div class="vh-head"><div><h2>Daily history</h2>
      <div class="micro">Daily solar yield from the SmartSolar charger, segmented by charge stage. Scroll for older days.</div></div>
      <div class="micro" id="vhd-status">waiting for the first connected read</div>
    </div>
    <div class="vh-legend">
      <span><i class="vh-swatch" style="background:#f4791f"></i>Bulk</span>
      <span><i class="vh-swatch" style="background:#e3bc4f"></i>Absorption</span>
      <span><i class="vh-swatch" style="background:#6aaed6"></i>Float</span>
      <span id="vhd-peak"></span>
    </div>
    <div class="vh-detailwrap"><div class="vh-detail" id="vh-detail"><div class="micro" style="padding:6px">Waiting for charger history</div></div></div>
  </div>
  <div class="card"><div class="vh-head"><div><h2 id="vh-daytitle">Today</h2>
      <div class="micro">Detail for the selected day &mdash; tap a bar in the history above to switch days.</div></div>
      <div class="micro" id="vhist-status">waiting for the first connected read</div>
    </div>
    <div class="vh-daystats" id="vh-daystats"><div class="micro">Waiting for charger history</div></div>
    <div class="vh-errors micro" id="vh-errors">Error history waiting</div>
  </div>
  <div class="card"><h2>30-day trends</h2>
    <div class="micro">The same daily records as the history table, charted over the month (today at left, oldest at right).</div>
    <div class="chartgrid">
      <div class="chartpanel"><h3>Load consumption</h3><div class="micro">Daily energy used by the load output (kWh)</div><div class="cwrap"><canvas id="vh-consumed" class="chart compact"></canvas></div></div>
      <div class="chartpanel"><h3>Peak solar power</h3><div class="micro">Highest panel power each day (W)</div><div class="cwrap"><canvas id="vh-peak" class="chart compact"></canvas></div></div>
      <div class="chartpanel"><h3>Maximum panel voltage</h3><div class="micro">Highest PV voltage each day (V)</div><div class="cwrap"><canvas id="vh-pvmax" class="chart compact"></canvas></div></div>
      <div class="chartpanel"><h3>Battery voltage range</h3><div class="micro">Daily minimum to maximum battery voltage (V)</div><div class="cwrap"><canvas id="vh-battery" class="chart compact"></canvas></div></div>
      <div class="chartpanel"><h3>Maximum charge current</h3><div class="micro">Highest battery charge current each day (A)</div><div class="cwrap"><canvas id="vh-imax" class="chart compact"></canvas></div></div>
      <div class="chartpanel"><h3>Charge-stage duration</h3><div class="micro">Bulk, absorption, and float minutes per day</div><div class="cwrap"><canvas id="vh-stages" class="chart compact"></canvas></div></div>
    </div>
  </div>
  <div class="card"><h2>EcoWorthy / JBD 280 Ah</h2>
    <div class="grid">
      <div>
        <div class="row"><span class="k">SOC</span><span class="v"><span id="p-soc">--</span>%</span></div>
        <div class="row"><span class="k">Status</span><span class="v" id="p-bst">--</span></div>
        <div class="row"><span class="k">Total voltage</span><span class="v" id="p-bv">--</span></div>
        <div class="row"><span class="k">Total current</span><span class="v" id="p-ba">--</span></div>
        <div class="row"><span class="k">Power</span><span class="v" id="p-bw">--</span></div>
        <div class="row"><span class="k">Remaining capacity</span><span class="v" id="p-brc">--</span></div>
        <div class="row"><span class="k">Available capacity</span><span class="v" id="p-bnc">--</span></div>
      </div>
      <div>
        <div class="row"><span class="k">Remaining time</span><span class="v" id="p-bwt">--</span></div>
        <div class="row"><span class="k">Temperature</span><span class="v" id="p-btm">--</span></div>
        <div class="row"><span class="k">Cell count</span><span class="v" id="p-bct">--</span></div>
        <div class="row"><span class="k">Cycles</span><span class="v" id="p-bcy">--</span></div>
        <div class="row"><span class="k">FET flags</span><span class="v" id="p-bfet">--</span></div>
        <div class="row"><span class="k">Protection</span><span class="v" id="p-bpro">--</span></div>
        <div class="row"><span class="k">SW version</span><span class="v" id="p-bsw">--</span></div>
      </div>
    </div>
    <div class="k" style="margin-top:10px">Single cell voltage</div>
    <div id="p-cells" class="cells"></div>
  </div>
  <div class="card"><h2>Victron SmartSolar MPPT</h2>
    <div class="grid">
      <div>
        <div class="row"><span class="k">Model</span><span class="v" id="p-smodel">--</span></div>
        <div class="row"><span class="k">Serial</span><span class="v" id="p-sserial">--</span></div>
        <div class="row"><span class="k">Firmware</span><span class="v" id="p-sfw">--</span></div>
        <div class="row"><span class="k">PV watts</span><span class="v" id="p-pv">--</span></div>
        <div class="row"><span class="k">PV voltage</span><span class="v" id="p-pvv">--</span></div>
        <div class="row"><span class="k">PV current</span><span class="v" id="p-pva">--</span></div>
        <div class="row"><span class="k">Peak today</span><span class="v" id="p-pvmax">--</span></div>
        <div class="row"><span class="k">Peak yesterday</span><span class="v" id="p-pvmaxy">--</span></div>
        <div class="row"><span class="k">31-day peak</span><span class="v" id="p-pvmonth">--</span></div>
        <div class="row"><span class="k">Current / 31-day peak</span><span class="v" id="p-pct">--</span></div>
        <div class="row"><span class="k">Charger state</span><span class="v" id="p-state">--</span></div>
      </div>
      <div>
        <div class="row"><span class="k">Battery V</span><span class="v" id="p-sv">--</span></div>
        <div class="row"><span class="k">Battery A</span><span class="v" id="p-sa">--</span></div>
        <div class="row"><span class="k">Battery temp</span><span class="v" id="p-stemp">--</span></div>
        <div class="row"><span class="k">Battery range today</span><span class="v" id="p-srange">--</span></div>
        <div class="row"><span class="k">Load A</span><span class="v" id="p-sla">--</span></div>
        <div class="row"><span class="k">Load output</span><span class="v" id="p-sload">--</span></div>
        <div class="row"><span class="k">Yield today</span><span class="v" id="p-yd">--</span></div>
        <div class="row"><span class="k">Yield yesterday</span><span class="v" id="p-ydy">--</span></div>
        <div class="row"><span class="k">Lifetime yield</span><span class="v" id="p-yt">--</span></div>
        <div class="row"><span class="k">RSSI</span><span class="v" id="p-srssi">--</span></div>
      </div>
    </div>
    <div class="micro" id="p-sconnected" style="margin-top:8px">Extended read waiting</div>
    <div class="micro" id="p-unkvreg" style="margin-top:4px"></div>
  </div>
</section>
<section id="display">
  <div class="card"><h2>Display Settings</h2>
    <label>Editing page</label><select id="dp-page" onchange="paintDisplay()"></select>
    <div class="preview" id="dp-prev"><img id="dp-img"><div class="box" id="dp-boxel">Time</div></div>
    <label style="margin-top:12px">Background</label>
    <select id="dp-bg" onchange="dpControlChanged('bg')"></select>
    <form id="dp-form" onsubmit="uploadBg(event)" style="margin-top:8px">
      <input id="dp-file" type="file" accept="image/jpeg">
      <button class="acc" type="submit" style="margin-top:6px;width:100%">Upload JPEG</button>
    </form>
    <div class="grid" style="margin-top:8px">
      <div><label>Color theme</label><select id="dp-theme" onchange="dpControlChanged('theme')"></select></div>
      <div><label>Contrast helper</label><select id="dp-contrast" onchange="dpControlChanged('contrast')">
        <option value=0>None</option><option value=1>Dark text</option><option value=2>Dark text + box</option>
      </select></div>
    </div>
    <label style="margin-top:8px"><input type="checkbox" id="dp-box" style="width:auto" onchange="dpControlChanged('box')"> Data box (translucent panel behind text)</label>
    <div class="grid">
      <div><label>Scale (%)</label><input id="dp-s" type="range" min="70" max="150" oninput="dpControlChanged('scale')"></div>
      <div><label>Position</label><span class="pill" id="dp-svalue">100%</span> <span class="pill" id="dp-xyvalue">0,0</span></div>
    </div>
    <div class="micro" style="margin-top:6px">Drag the highlighted box in the preview above to reposition.</div>
    <div class="grid" style="margin-top:10px">
      <button onclick="resetPage()">Reset this page</button>
      <button class="danger" onclick="resetAllPages()">Reset all pages</button>
    </div>
    <div class="micro">Night clock layout is locked. Reset restores background, theme, box, contrast, and layout for the selected page.</div>
  </div>
  <div class="card"><h2>Seasonal Presets</h2>
    <div id="pre-list" class="rowlist"><div class="k">loading&hellip;</div></div>
    <div class="grid" style="margin-top:8px">
      <input id="pre-name" placeholder="Preset name (e.g. Christmas)" maxlength=32>
      <button class="acc" onclick="savePreset()">Save current as preset</button>
    </div>
    <div class="micro">Presets capture backgrounds, themes, boxes, contrast, layout, and day theme for all four display pages.</div>
  </div>
  <div class="card"><h2>Display &amp; Behavior</h2>
    <label>Color scheme (default day theme)</label><select id="s-theme" onchange="saveAll()"></select>
    <label>Day brightness</label><input id="s-bright" type="range" min="10" max="255" oninput="saveAll()">
    <label style="margin-top:8px"><input type="checkbox" id="s-anim" style="width:auto" onchange="saveAll()"> Animations</label>
    <label style="margin-top:14px">Night brightness</label><input id="s-nbright" type="range" min="5" max="150" oninput="saveAll()">
    <label style="margin-top:8px"><input type="checkbox" id="s-autoNight" style="width:auto" onchange="saveAll()"> Auto night mode (black face, white digits) on the home page</label>
    <div class="grid"><div><label>Night starts</label><select id="s-nStart" onchange="saveAll()"></select></div>
     <div><label>Night ends</label><select id="s-nEnd" onchange="saveAll()"></select></div></div>
    <label>Return-to-clock timeout (seconds)</label><input id="s-hto" type="number" min="10" max="600" onchange="saveAll()">
    <label style="margin-top:10px"><input type="checkbox" id="s-doEn" style="width:auto" onchange="saveAll()"> Turn the screen off on a schedule (wakes on touch)</label>
    <div class="grid"><div><label>Off from</label><select id="s-doStart" onchange="saveAll()"></select></div>
     <div><label>Off until</label><select id="s-doEnd" onchange="saveAll()"></select></div></div>
    <label style="margin-top:10px">Storing protection (&deg;F) &mdash; used in Storing mode</label>
    <div class="grid"><div><label>Heat below</label><input id="s-stMin" type="number" min="20" max="60" onchange="saveAll()"></div>
     <div><label>Cool above</label><input id="s-stMax" type="number" min="70" max="110" onchange="saveAll()"></div></div>
  </div>
</section>
<section id="net">
  <div class="card"><h2>Networking</h2>
    <label>Hostname (mDNS)</label>
    <div class="grid"><input id="nt-host" maxlength=24><button onclick="saveHost()">Apply</button></div>
    <div class="micro">Device appears at <b><span id="nt-hostshow">huckleberry</span>.local</b>. Also always at the <b>Huckleberry</b> Wi-Fi hotspot for off-grid.</div>
    <label style="margin-top:14px">Saved Wi-Fi networks (home + campsites)</label>
    <div id="nt-list" class="rowlist"><div class="k">none yet</div></div>
    <div class="grid" style="margin-top:8px">
      <input id="nt-ssid" placeholder="SSID"><input id="nt-pass" type="password" placeholder="password">
    </div>
    <button class="acc" style="margin-top:8px;width:100%" onclick="addNet()">Add network</button>
    <div class="micro" style="margin-top:8px">The trailer auto-joins the first saved network it finds.</div>
    <label style="margin-top:14px">Current state</label>
    <div class="row"><span class="k">STA</span><span class="v" id="nt-sta">--</span></div>
    <div class="row"><span class="k">SSID</span><span class="v" id="nt-cur">--</span></div>
    <div class="row"><span class="k">IP</span><span class="v" id="nt-ip">--</span></div>
    <div class="row"><span class="k">AP</span><span class="v" id="nt-ap">--</span></div>
    <div class="row"><span class="k">Time source</span><span class="v" id="nt-tsrc">--</span></div>
  </div>
  <div class="card"><h2>Web Theme</h2>
    <label>Accent color</label>
    <div class="grid">
      <input id="wa-color" type="color">
      <button onclick="clearAccent()">Reset to default</button>
    </div>
    <div class="micro">Recolors the web UI only. The on-device clock uses the display color theme.</div>
  </div>
</section>
<section id="ble">
  <div class="card"><h2>Battery (EcoWorthy / JBD)</h2>
    <label>MAC address</label>
    <div class="grid"><input id="b-mac" placeholder="aa:bb:cc:dd:ee:ff"><button onclick="saveBle()">Save</button></div>
    <div class="row"><span class="k">Link</span><span class="v" id="b-link">--</span></div>
    <div class="row"><span class="k">Last basic</span><span class="v" id="b-basic">--</span></div>
    <div class="row"><span class="k">Last cell frame</span><span class="v" id="b-cell">--</span></div>
  </div>
  <div class="card"><h2>Solar (Victron MPPT)</h2>
    <label>MAC address</label><input id="v-mac" placeholder="aa:bb:cc:dd:ee:ff">
    <label>Encryption key (16 bytes hex)</label>
    <div class="grid"><input id="v-key" type="password" placeholder="hex"><button onclick="saveBle()">Save</button></div>
    <label>Bluetooth pairing PIN</label>
    <input id="v-pin" type="password" inputmode="numeric" maxlength="6" placeholder="6 digits from device label">
    <div class="row"><span class="k">Link</span><span class="v" id="v-link">--</span></div>
    <div class="row"><span class="k">RSSI</span><span class="v" id="v-rssi">--</span></div>
    <div class="row"><span class="k">Last readout</span><span class="v" id="v-last">--</span></div>
    <div class="micro" style="margin-top:10px">Live solar data uses low-duty passive advertisements. Charger-owned history refreshes after Wi-Fi is stable, then every six hours.</div>
  </div>
  <div class="card"><h2>AC (Gidrox)</h2>
    <label>MAC address</label>
    <div class="grid"><input id="g-mac" placeholder="unpaired"><button onclick="saveBle()">Save</button></div>
    <div class="micro">Gidrox 10k BTU pairing arrives with the unit &mdash; see roadmap M4.</div>
  </div>
  <div class="card"><h2>Global</h2>
    <label style="margin-top:6px"><input type="checkbox" id="b-en" style="width:auto" onchange="saveBle()"> BLE enabled</label>
    <div class="micro">Later: scan nearby BLE devices and pick from a list.</div>
  </div>
</section>
<section id="fw">
  <div class="card"><h2>Firmware</h2>
    <div class="row"><span class="k">Version</span><span class="v" id="f-ver">--</span></div>
    <div class="row"><span class="k">Free heap</span><span class="v" id="f-heap">--</span></div>
    <div class="row"><span class="k">Uptime</span><span class="v" id="f-up">--</span></div>
    <p style="margin-top:12px"><a href="/update"><button class="acc" style="width:100%">Browser OTA (upload .bin)</button></a></p>
    <label style="margin-top:8px">CLI paths</label>
    <div class="micro">Firmware over Wi-Fi: <code>pio run -e huckleberry_ota -t upload</code></div>
    <div class="micro">Backgrounds (SPIFFS): <code>pio run -e huckleberry -t uploadfs</code></div>
    <div class="micro">OTA password: <b>huckleberry</b>. Hostname: <code>huckleberry.local</code>.</div>
  </div>
  <div class="card"><h2>Release notes</h2>
    <div class="k" id="f-notes"></div>
  </div>
</section>
</div>
<script>
const TABS=['dashboard','power','display','net','ble','fw'];
const PAGE_NAMES=['Clock','Climate','Power','Status'];
const PAGE_LAYOUT_SLOT=[0,2,3,5];
const PAGE_LAYOUT_DEFAULT=[{x:14,y:30,scale:100},{x:12,y:72,scale:100},{x:12,y:70,scale:100},{x:164,y:46,scale:100}];
// Per-page widget dimensions at scale 100 (mirrors server-side clampLayoutForSlot).
// Order: Clock -> DAY_CLOCK(0), Climate -> THERMO_CARD(2), Power -> POWER_GAUGE(3), Status -> STATUS_CARD(5).
const PAGE_WIDGET_DIMS=[[252,130],[158,132],[294,138],[154,152]];
const PAGE_SCALE_MIN=[70,75,75,75];
// Actual max = floor(min(320/w, 240/h)*100) per page's primary widget.
const PAGE_SCALE_MAX=[126,181,108,157];
const PAGE_BG_DEFAULT=['bg_indie_02.jpg','bg_charlie_01.jpg','bg_creek_01.jpg','bg_indie_01.jpg'];
const RELEASE_NOTES='v0.4.0 (Victron power center): connected SmartSolar reads over BLE (PIN passkey) add serial, firmware, yesterday yield/peak and more. Power page redesigned — round gauges up top, a VictronConnect-style daily-history table (tap a day for details), 30-day trend charts, then EcoWorthy/MPPT detail at the bottom. Inside temperature from the pack sensor now shows on the clock, dashboard, and thermostat. Contrast helper reworked into a consistent dark scrim. Extended data refreshes every 15 min. Also v0.3.0 (M2.1): six-section web app, display preview/presets, configurable accent.';
let S={},VH={days:[]},themeInit=0,userTyping=null,lastVictronConnectedAge=null;
function $(i){return document.getElementById(i)}
function setGauge(id,pct){let el=$(id);if(el)el.style.setProperty('--pct',String(320*Math.max(0,Math.min(1,pct||0))))}
function setAccent(id,col){let el=$(id);if(el)el.style.setProperty('--accent',col)}
function paintGauges(){
  // Battery SOC
  let soc=(S.batt&&S.batt.soc>=0)?S.batt.soc:null;
  $('gv-soc').textContent=soc!=null?soc:'--';setGauge('g-soc',soc!=null?soc/100:0);
  // Solar in — full-scale follows the 31-day peak (min 150 W)
  let pv=(S.sol&&S.sol.valid&&S.sol.pv!=null)?S.sol.pv:null;
  let pvMax=Math.max(Math.round((S.sol&&S.sol.monthPeak)||0),150);
  $('gv-pv').textContent=pv!=null?Math.round(pv):'--';$('gs-pv').textContent=pvMax;
  setGauge('g-pv',pv!=null?pv/pvMax:0);
  // Battery flow — signed watts, green charging / orange using, scale ±500 W
  let bw=(S.batt&&S.batt.w!=null)?S.batt.w:null;
  $('gv-bw').textContent=bw!=null?((bw>=0?'+':'')+Math.round(bw)):'--';
  setAccent('g-bw',bw==null?'#8a7c63':(bw>=0.5?'var(--ok)':(bw<=-0.5?'var(--acc)':'#8a7c63')));
  setGauge('g-bw',bw!=null?Math.abs(bw)/500:0);
  // Load out — amps, scale 0..15 (75/15 load output)
  let la=(S.sol&&S.sol.loadA!=null)?S.sol.loadA:null;
  $('gv-load').textContent=la!=null?la.toFixed(1):'--';
  setAccent('g-load',(S.sol&&S.sol.loadOn===false)?'#8a7c63':'var(--acc2)');
  setGauge('g-load',la!=null?la/15:0);
}
function fmt(x,d,u){return (x==null||isNaN(x))?'--':(x.toFixed(d)+(u||''))}
function fmtTime(h){if(h==null||isNaN(h))return'--';let m=Math.round(h*60);return Math.floor(m/60)+'h '+String(m%60).padStart(2,'0')+'m'}
// Victron firmware version: the hex digits read as a BCD-style version. 24-bit
// values (>0xffff) carry a release-type low byte (0x00/0xff = official) that is
// dropped, leaving 0xMMmm -> vMM.mm (e.g. 0x017400 -> v1.74).
function fmtFw(n){if(!n)return'--';let v=n>0xffff?(n>>8):n;let h=v.toString(16).padStart(4,'0');return 'v'+parseInt(h.slice(0,-2),10)+'.'+h.slice(-2)}
function fmtUp(s){let h=Math.floor(s/3600),m=Math.floor((s%3600)/60);return h+'h '+String(m).padStart(2,'0')+'m'}
function activeTab(){let h=(location.hash||'#dashboard').substr(1);return TABS.includes(h)?h:'dashboard'}
function switchTab(t){location.hash='#'+t;render()}
function render(){
  let cur=activeTab();
  TABS.forEach(t=>{let s=$(t);if(s)s.classList.toggle('on',t===cur)});
  document.querySelectorAll('nav button').forEach(b=>b.classList.toggle('on',b.dataset.tab===cur));
  paint();
  if(cur==='power'){loadVictronHistory();requestAnimationFrame(drawVictronHistory)}
}
async function load(){try{S=await (await fetch('/api/state')).json();paint()}catch(e){}}
async function loadVictronHistory(){try{VH=await (await fetch('/api/victron/history')).json();if(activeTab()==='power')drawVictronHistory()}catch(e){}}
const VH_COLORS={yield:'#f4791f',consumed:'#9b7bc8',peak:'#d9bd55',pvmax:'#e0aa55',bmin:'#6aaed6',bmax:'#8f7bd6',imax:'#7db56a',bulk:'#f4791f',abs:'#e3bc4f',float:'#6aaed6'};
let selectedVictronAge=0,victronBarHits=[];
function victronDays(){return(VH.days||[]).slice().sort((a,b)=>a.age-b.age)}
function victronAgeLabel(age){return age===0?'Today':age===1?'Yesterday':age+' days ago'}
function victronShortAge(age){return age===0?'Today':'-'+age+'d'}
function fmtMinutes(minutes){minutes=Math.max(0,Math.round(minutes||0));let h=Math.floor(minutes/60),m=minutes%60;return h?(h+'h '+String(m).padStart(2,'0')+'m'):(m+'m')}
function chartSetup(id,height,count){
  let canvas=$(id);if(!canvas)return null;
  let wrap=canvas.parentElement;
  let avail=Math.max(250,Math.round((wrap&&wrap.clientWidth)||canvas.clientWidth||600));
  // Give each day a minimum width; when the total exceeds the viewport the
  // canvas grows past its .cwrap wrapper and scrolls horizontally (nice on
  // small/portrait screens), while on wide screens it just fills the width.
  let width=count?Math.max(avail,52+count*15):avail;
  canvas.style.width=width+'px';
  let dpr=Math.min(2,window.devicePixelRatio||1);
  if(canvas.width!==Math.round(width*dpr)||canvas.height!==Math.round(height*dpr)){canvas.width=Math.round(width*dpr);canvas.height=Math.round(height*dpr)}
  let ctx=canvas.getContext('2d');ctx.setTransform(dpr,0,0,dpr,0,0);ctx.clearRect(0,0,width,height);
  return{canvas,ctx,width,height,left:42,right:10,top:14,bottom:27,plotW:width-52,plotH:height-41};
}
function chartEmpty(id,height,message){let f=chartSetup(id,height);if(!f)return;f.ctx.fillStyle='#8a7c63';f.ctx.font='12px system-ui';f.ctx.fillText(message||'Waiting for charger history',14,27)}
function chartScale(values,zero){
  let min=Math.min(...values),max=Math.max(...values);
  if(zero){min=0;max=max>0?max*1.08:1}else if(max-min<0.001){let pad=Math.max(.1,Math.abs(max)*.03);min-=pad;max+=pad}else{let pad=(max-min)*.1;min-=pad;max+=pad}
  return{min,max,y:(value,frame)=>frame.top+(max-value)/(max-min)*frame.plotH};
}
function drawChartGrid(frame,scale,digits){
  let ctx=frame.ctx;ctx.strokeStyle='#2d2113';ctx.fillStyle='#8a7c63';ctx.lineWidth=1;ctx.font='9px system-ui';ctx.textAlign='right';
  for(let i=0;i<=3;i++){let y=frame.top+frame.plotH*i/3;ctx.beginPath();ctx.moveTo(frame.left,y);ctx.lineTo(frame.width-frame.right,y);ctx.stroke();ctx.fillText((scale.max-(scale.max-scale.min)*i/3).toFixed(digits),frame.left-5,y+3)}
  ctx.textAlign='start';
}
function drawDayLabels(frame,points){
  let ctx=frame.ctx,count=points.length,step=frame.plotW/count,every=Math.max(1,Math.ceil(count/6));ctx.fillStyle='#8a7c63';ctx.font='9px system-ui';ctx.textAlign='center';
  points.forEach((point,index)=>{if(index%every===0||index===count-1)ctx.fillText(victronShortAge(point.age),frame.left+step*(index+.5),frame.height-8)});ctx.textAlign='start';
}
function drawMetricBars(id,key,color,digits){
  let points=victronDays().filter(point=>Number.isFinite(point[key]));if(!points.length){chartEmpty(id,176);return}
  let frame=chartSetup(id,176,points.length),scale=chartScale(points.map(point=>point[key]),true),ctx=frame.ctx;drawChartGrid(frame,scale,digits);
  let step=frame.plotW/points.length,barWidth=Math.max(3,Math.min(18,step*.64)),bottom=frame.top+frame.plotH;
  points.forEach((point,index)=>{let x=frame.left+step*(index+.5),y=scale.y(point[key],frame);ctx.globalAlpha=point.age===selectedVictronAge?1:.76;ctx.fillStyle=color;ctx.fillRect(x-barWidth/2,y,barWidth,Math.max(1,bottom-y));if(point.age===selectedVictronAge){ctx.strokeStyle='#ffe9c8';ctx.strokeRect(x-barWidth/2-1,y-1,barWidth+2,Math.max(2,bottom-y+2))}});ctx.globalAlpha=1;drawDayLabels(frame,points);
}
function drawVictronOverview(days){
  if(!days.length){chartEmpty('vh-overview',260);victronBarHits=[];return}
  let points=days.filter(point=>Number.isFinite(point.yield));if(!points.length){chartEmpty('vh-overview',260);victronBarHits=[];return}
  let frame=chartSetup('vh-overview',260),scale=chartScale(points.map(point=>point.yield),true),ctx=frame.ctx;drawChartGrid(frame,scale,2);
  let step=frame.plotW/points.length,barWidth=Math.max(5,Math.min(22,step*.7)),bottom=frame.top+frame.plotH;victronBarHits=[];
  points.forEach((point,index)=>{
    let x=frame.left+step*(index+.5),top=scale.y(point.yield,frame),barHeight=Math.max(2,bottom-top),total=(point.bulk||0)+(point.abs||0)+(point.float||0),cursor=bottom;
    let segments=total?[[point.bulk||0,VH_COLORS.bulk],[point.abs||0,VH_COLORS.abs],[point.float||0,VH_COLORS.float]]:[[1,'#5b4933']];
    segments.forEach((segment,segmentIndex)=>{let segmentHeight=segmentIndex===segments.length-1?cursor-top:barHeight*segment[0]/(total||1);cursor-=segmentHeight;ctx.fillStyle=segment[1];ctx.fillRect(x-barWidth/2,cursor,barWidth,Math.max(1,segmentHeight))});
    if(point.age===selectedVictronAge){ctx.strokeStyle='#ffe9c8';ctx.lineWidth=2;ctx.strokeRect(x-barWidth/2-2,top-2,barWidth+4,barHeight+4)}
    if(points.length<=10){ctx.fillStyle='#cbb38c';ctx.font='9px system-ui';ctx.textAlign='center';ctx.fillText(point.yield.toFixed(2),x,Math.max(10,top-5))}
    victronBarHits.push({age:point.age,x:x});
  });
  ctx.textAlign='start';drawDayLabels(frame,points);
}
function drawBatteryRange(){
  let points=victronDays().filter(point=>Number.isFinite(point.bmin)&&Number.isFinite(point.bmax));if(!points.length){chartEmpty('vh-battery',176);return}
  let frame=chartSetup('vh-battery',176,points.length),scale=chartScale(points.flatMap(point=>[point.bmin,point.bmax]),false),ctx=frame.ctx;drawChartGrid(frame,scale,2);
  let step=frame.plotW/points.length;
  points.forEach((point,index)=>{let x=frame.left+step*(index+.5),yMin=scale.y(point.bmin,frame),yMax=scale.y(point.bmax,frame);ctx.strokeStyle=point.age===selectedVictronAge?'#ffe9c8':VH_COLORS.bmax;ctx.lineWidth=5;ctx.beginPath();ctx.moveTo(x,yMin);ctx.lineTo(x,yMax);ctx.stroke();ctx.fillStyle=VH_COLORS.bmin;ctx.beginPath();ctx.arc(x,yMin,3,0,Math.PI*2);ctx.fill();ctx.fillStyle=VH_COLORS.bmax;ctx.beginPath();ctx.arc(x,yMax,3,0,Math.PI*2);ctx.fill()});drawDayLabels(frame,points);
}
function drawStageBars(){
  let points=victronDays();if(!points.length){chartEmpty('vh-stages',176);return}
  let totals=points.map(point=>(point.bulk||0)+(point.abs||0)+(point.float||0)),frame=chartSetup('vh-stages',176,points.length),scale=chartScale(totals,true),ctx=frame.ctx;drawChartGrid(frame,scale,0);
  let step=frame.plotW/points.length,barWidth=Math.max(3,Math.min(18,step*.64)),bottom=frame.top+frame.plotH;
  points.forEach((point,index)=>{let x=frame.left+step*(index+.5),cursor=bottom;[[point.bulk||0,VH_COLORS.bulk],[point.abs||0,VH_COLORS.abs],[point.float||0,VH_COLORS.float]].forEach(segment=>{let height=segment[0]/(scale.max-scale.min)*frame.plotH;cursor-=height;ctx.fillStyle=segment[1];ctx.fillRect(x-barWidth/2,cursor,barWidth,Math.max(segment[0]?1:0,height))});if(point.age===selectedVictronAge){let top=scale.y((point.bulk||0)+(point.abs||0)+(point.float||0),frame);ctx.strokeStyle='#ffe9c8';ctx.strokeRect(x-barWidth/2-1,top-1,barWidth+2,Math.max(2,bottom-top+2))}});drawDayLabels(frame,points);
}
function renderVictronDay(days){
  let day=days.find(point=>point.age===selectedVictronAge)||(days.find(point=>point.age===0)||days[days.length-1]);if(!day){$('vh-daytitle').textContent='Daily details';$('vh-daystats').innerHTML='<div class="micro">Waiting for charger history</div>';$('vh-errors').textContent='Error history waiting';return}
  selectedVictronAge=day.age;let total=(day.bulk||0)+(day.abs||0)+(day.float||0),percent=value=>total?Math.round((value||0)*100/total)+'%':'--';
  let errors=(day.errors||[]).filter(value=>value),errorText=errors.length?errors.join(', '):'None';
  let stat=(label,value)=>`<div class=vh-stat><span>${label}</span><b>${value}</b></div>`;
  $('vh-daytitle').textContent=victronAgeLabel(day.age)+' - charger day '+day.seq;
  $('vh-daystats').innerHTML=stat('Solar yield',fmt(day.yield,2,' kWh'))+stat('Load consumption',fmt(day.consumed,2,' kWh'))+stat('Peak power',fmt(day.peak,0,' W'))+stat('Maximum PV voltage',fmt(day.pvmax,2,' V'))+stat('Battery voltage',fmt(day.bmin,2,' V')+' to '+fmt(day.bmax,2,' V'))+stat('Maximum charge current',fmt(day.imax,1,' A'))+stat('Bulk',fmtMinutes(day.bulk)+' ('+percent(day.bulk)+')')+stat('Absorption',fmtMinutes(day.abs)+' ('+percent(day.abs)+')')+stat('Float',fmtMinutes(day.float)+' ('+percent(day.float)+')')+stat('Charger errors',errorText);
  let errorDays=days.map(point=>({point,codes:(point.errors||[]).filter(value=>value)})).filter(item=>item.codes.length);
  $('vh-errors').innerHTML=errorDays.length?('<span class=bad>Stored charger errors:</span> '+errorDays.map(item=>victronAgeLabel(item.point.age)+' ['+item.codes.join(', ')+']').join(' &middot; ')):'No charger errors in the stored history.';
}
function fmtWh(kwh){if(kwh==null||isNaN(kwh))return'--';return kwh>=1?kwh.toFixed(2)+'kWh':Math.round(kwh*1000)+'Wh'}
function errCell(e){if(e==null)return'--';if(Array.isArray(e)){let x=e.filter(v=>v);return x.length?x.map(v=>'#'+v).join(' '):'--'}return e?('#'+e):'--'}
// VictronConnect-style "Detailed" view: stacked daily yield bars with an
// aligned per-day text table (Yield / P max / V max / Batt max / min / Errors).
function drawVictronDetail(){
  let wrap=$('vh-detail'); if(!wrap) return;
  let days=(VH.days||[]).slice().filter(d=>Number.isFinite(d.yield)).sort((a,b)=>a.age-b.age);
  let st=$('vhd-status'),pk=$('vhd-peak');
  if(!days.length){wrap.innerHTML='<div class="micro" style="padding:6px">Waiting for charger history</div>';wrap.style.gridTemplateColumns='';if(st)st.textContent='waiting for the first connected read';if(pk)pk.textContent='';return}
  let maxY=Math.max.apply(null,days.map(d=>d.yield||0).concat(0.001));
  if(st)st.textContent=days.length+' days';
  if(pk)pk.textContent='peak day '+fmtWh(maxY);
  wrap.style.gridTemplateColumns='max-content repeat('+days.length+',minmax(56px,1fr))';
  let seg=(m,tot,c)=>'<div class="seg" style="height:'+(tot?(m/tot*100):0).toFixed(1)+'%;background:'+c+'"></div>';
  let h='<div class="lbl"></div>';
  days.forEach(d=>{let tot=(d.bulk||0)+(d.abs||0)+(d.float||0);
    let inner=tot?(seg(d.float||0,tot,'#6aaed6')+seg(d.abs||0,tot,'#e3bc4f')+seg(d.bulk||0,tot,'#f4791f')):'<div class="seg" style="height:100%;background:#5b4933"></div>';
    let sel=d.age===selectedVictronAge?' sel':'';
    h+='<div class="barcell'+sel+'" onclick="selectVictronDay('+d.age+')"><div class="bar" style="height:'+((d.yield||0)/maxY*100).toFixed(1)+'%">'+inner+'</div></div>';});
  h+='<div class="lbl"></div>';days.forEach(d=>{let sel=d.age===selectedVictronAge?' sel':'';h+='<div class="hd'+sel+'" onclick="selectVictronDay('+d.age+')">'+victronShortAge(d.age)+'</div>'});
  h+='<div class="grouphd">Solar panel</div>';
  h+='<div class="lbl">Yield</div>';days.forEach(d=>{h+='<div class="cell">'+fmtWh(d.yield)+'</div>'});
  h+='<div class="lbl">P max</div>';days.forEach(d=>{h+='<div class="cell">'+(Number.isFinite(d.peak)?Math.round(d.peak)+'W':'--')+'</div>'});
  h+='<div class="lbl">V max</div>';days.forEach(d=>{h+='<div class="cell">'+fmt(d.pvmax,2,'V')+'</div>'});
  h+='<div class="grouphd">Battery</div>';
  h+='<div class="lbl">max</div>';days.forEach(d=>{h+='<div class="cell">'+fmt(d.bmax,2,'V')+'</div>'});
  h+='<div class="lbl">min</div>';days.forEach(d=>{h+='<div class="cell">'+fmt(d.bmin,2,'V')+'</div>'});
  h+='<div class="grouphd">Errors</div>';
  h+='<div class="lbl">code</div>';days.forEach(d=>{h+='<div class="cell">'+errCell(d.errors)+'</div>'});
  wrap.innerHTML=h;
}
function drawVictronHistory(){
  drawVictronDetail();
  let days=victronDays();renderVictronDay(days);
  drawMetricBars('vh-consumed','consumed',VH_COLORS.consumed,2);drawMetricBars('vh-peak','peak',VH_COLORS.peak,0);drawMetricBars('vh-pvmax','pvmax',VH_COLORS.pvmax,1);drawBatteryRange();drawMetricBars('vh-imax','imax',VH_COLORS.imax,1);drawStageBars();
  let status=$('vhist-status');
  if(status)status.textContent=days.length?(days.length+' charger day'+(days.length===1?'':'s')+' available'):'waiting for the first connected read';
}
function selectVictronDay(age){selectedVictronAge=age;drawVictronHistory();}
function initSelects(){
  if(themeInit||!S.themes)return;
  $('s-theme').innerHTML=S.themes.map((n,i)=>`<option value=${i}>${n}</option>`).join('');
  $('dp-theme').innerHTML='<option value="-1">Color scheme</option>'+S.themes.map((n,i)=>`<option value=${i}>${n}</option>`).join('');
  $('dp-page').innerHTML=PAGE_NAMES.map((n,i)=>`<option value=${i}>${n}</option>`).join('');
  let hrs='';for(let h=0;h<24;h++){let ap=h<12?'AM':'PM',hh=h%12||12;hrs+=`<option value=${h}>${hh} ${ap}</option>`}
  ['s-nStart','s-nEnd','s-doStart','s-doEnd'].forEach(id=>$(id).innerHTML=hrs);
  themeInit=1;
}
function accent(){
  let a=(S.set&&S.set.wAcc)||'';
  document.documentElement.style.setProperty('--acc',a||'#F4791F');
  if($('wa-color')&&document.activeElement!==$('wa-color'))$('wa-color').value=a||'#f4791f';
}
function paint(){
  if(!S||!S.set)return;
  initSelects();accent();
  if(S.time)$('clk').textContent=S.time;
  // Dashboard
  $('d-soc').textContent=S.batt&&S.batt.soc>=0?S.batt.soc:'--';
  $('d-btp').textContent=fmt(S.batt&&S.batt.w,0,' W');
  $('d-pv').textContent=S.sol&&S.sol.valid?Math.round(S.sol.pv):'--';
  $('d-ss').textContent=S.sol?S.sol.state:'--';
  $('d-set').textContent=S.th.sp;$('d-mode').value=S.th.mode;$('d-camp').value=S.th.camp?1:0;
  $('d-inside').textContent=S.th.inside!=null?Math.round(S.th.inside):'--';
  $('d-wifi').innerHTML=S.net.sta?('<span class=ok>'+(S.net.ssid||'connected')+'</span>'):'<span class=bad>offline</span>';
  $('d-blink').innerHTML=S.batt&&S.batt.valid?'<span class=ok>live</span>':'waiting';
  $('d-slink').innerHTML=S.sol&&S.sol.valid?('<span class=ok>live '+S.sol.rssi+'dBm</span>'):'waiting';
  $('d-fw').textContent=S.fw||'--';
  // Power
  if(S.batt){
    $('p-soc').textContent=S.batt.soc>=0?S.batt.soc:'--';
    $('p-bst').textContent=S.batt.status||'--';
    $('p-bv').textContent=fmt(S.batt.v,2,' V');$('p-ba').textContent=fmt(S.batt.a,2,' A');
    $('p-bw').textContent=fmt(S.batt.w,0,' W');
    $('p-brc').textContent=fmt(S.batt.resid,2,' Ah');$('p-bnc').textContent=fmt(S.batt.nom,0,' Ah');
    $('p-bwt').textContent=fmtTime(S.batt.workH);
    let temps=S.batt.temps||[];$('p-btm').textContent=temps.length?temps.map(t=>fmt(t,1,'°F')).join(' / '):'--';
    $('p-bct').textContent=S.batt.cellCount>0?S.batt.cellCount:'--';
    $('p-bcy').textContent=S.batt.cycles>=0?S.batt.cycles:'--';
    $('p-bfet').textContent=S.batt.fet!=null?'0x'+S.batt.fet.toString(16):'--';
    $('p-bpro').textContent=S.batt.protect!=null?'0x'+S.batt.protect.toString(16):'--';
    $('p-bsw').textContent=S.batt.sw!=null?S.batt.sw:'--';
    let cells=S.batt.cellMv||[];
    $('p-cells').innerHTML=cells.length?cells.map((mv,i)=>`<div class=cell><div class=k>${String(i+1).padStart(2,'0')}</div><div class=v>${(mv/1000).toFixed(3)} V</div></div>`).join(''):'<div class=k>waiting</div>';
    $('b-basic').textContent=S.batt.valid?fmt(S.batt.v,2,' V')+' '+fmt(S.batt.a,2,' A'):'--';
    $('b-cell').textContent=cells.length?(cells.length+' cells, avg '+(cells.reduce((a,b)=>a+b,0)/cells.length/1000).toFixed(3)+' V'):'--';
  }
  if(S.sol){
    $('p-smodel').textContent=S.sol.model||'--';
    $('p-sserial').textContent=S.sol.serial||'--';
    $('p-sfw').textContent=S.sol.fw?fmtFw(S.sol.fw):'--';
    $('p-pv').textContent=S.sol.valid?Math.round(S.sol.pv)+' W':'--';
    $('p-pvv').textContent=fmt(S.sol.pvV,2,' V');
    $('p-pva').textContent=(S.sol.pv!=null&&S.sol.pvV>0.5)?fmt(S.sol.pv/S.sol.pvV,1,' A'):'--';
    $('p-pvmax').textContent=fmt(S.sol.peakToday,0,' W');
    $('p-pvmaxy').textContent=fmt(S.sol.peakYest,0,' W');
    $('p-stemp').textContent=S.sol.battTemp!=null?fmt(S.sol.battTemp,1,' °C'):'--';
    $('p-ydy').textContent=fmt(S.sol.yieldYest,2,' kWh');
    $('p-pvmonth').textContent=fmt(S.sol.monthPeak,0,' W');
    $('p-pct').textContent=S.sol.monthPct!=null?Math.round(S.sol.monthPct)+'%':'--';
    $('p-state').textContent=S.sol.state||'--';
    $('p-sv').textContent=fmt(S.sol.v,2,' V');$('p-sa').textContent=fmt(S.sol.a,2,' A');
    $('p-srange').textContent=(S.sol.bMin!=null&&S.sol.bMax!=null)?fmt(S.sol.bMin,2,'')+' - '+fmt(S.sol.bMax,2,' V'):'--';
    $('p-sla').textContent=fmt(S.sol.loadA,2,' A');
    $('p-sload').textContent=S.sol.loadOn==null?'--':((S.sol.loadOn?'On':'Off')+(S.sol.loadV!=null?' at '+fmt(S.sol.loadV,2,' V'):''));
    $('p-yd').textContent=fmt(S.sol.yield,2,' kWh');
    $('p-yt').textContent=fmt(S.sol.totalYield,2,' kWh');
    $('p-srssi').textContent=S.sol.valid?S.sol.rssi+' dBm':'--';
    $('p-sconnected').textContent=S.sol.connectedAge!=null?'Extended read '+Math.max(0,Math.round(S.sol.connectedAge/60))+' min ago':'Extended read waiting';
    $('p-unkvreg').textContent=(S.sol.unknownVregs&&S.sol.unknownVregs.length)?('Charger rejected VREGs: '+S.sol.unknownVregs.map(v=>'0x'+v.toString(16).padStart(4,'0')).join(', ')):'';
    $('v-link').innerHTML=S.sol.valid?'<span class=ok>live</span>':'waiting';
    $('v-rssi').textContent=S.sol.valid?(S.sol.rssi+' dBm'):'--';
    $('v-last').textContent=S.sol.valid?Math.round(S.sol.pv)+' W':'--';
    let connectedAge=S.sol.connectedAge;
    if(activeTab()==='power'&&connectedAge!=null&&(lastVictronConnectedAge==null||connectedAge+10<lastVictronConnectedAge))loadVictronHistory();
    if(connectedAge!=null)lastVictronConnectedAge=connectedAge;
  }
  $('b-link').innerHTML=S.batt&&S.batt.valid?'<span class=ok>live</span>':'waiting';
  paintGauges();
  // Network
  $('nt-sta').innerHTML=S.net.sta?'<span class=ok>connected</span>':'<span class=bad>offline</span>';
  $('nt-cur').textContent=S.net.ssid||'--';$('nt-ip').textContent=S.net.ip||'--';
  $('nt-ap').textContent=S.net.ap||'--';$('nt-tsrc').textContent=S.net.tsrc||'--';
  if(document.activeElement!==$('nt-host'))$('nt-host').value=(S.net.host||'huckleberry');
  $('nt-hostshow').textContent=S.net.host||'huckleberry';
  renderNets();
  // Behavior / defaults
  let s=S.set;
  $('s-theme').value=s.theme;$('s-bright').value=s.bright;$('s-anim').checked=s.anim;
  $('s-nbright').value=s.nbright;$('s-autoNight').checked=s.autoNight;
  $('s-nStart').value=s.nStart;$('s-nEnd').value=s.nEnd;$('s-hto').value=s.hto;
  $('s-doEn').checked=s.doEn;$('s-doStart').value=s.doStart;$('s-doEnd').value=s.doEnd;
  $('s-stMin').value=s.stMin;$('s-stMax').value=s.stMax;
  // BLE bindings
  if(S.ble&&document.activeElement!==$('b-mac'))$('b-mac').value=S.ble.bMac||'';
  if(S.ble&&document.activeElement!==$('v-mac'))$('v-mac').value=S.ble.vMac||'';
  if(S.ble&&document.activeElement!==$('v-key'))$('v-key').value=S.ble.vKey||'';
  if(S.ble&&document.activeElement!==$('v-pin')){$('v-pin').value='';$('v-pin').placeholder=S.ble.vPinSet?'PIN saved (enter to replace)':'6 digits from device label'}
  if(S.ble&&document.activeElement!==$('g-mac'))$('g-mac').value=S.ble.gMac||'';
  if(S.ble)$('b-en').checked=!!S.ble.en;
  // Firmware
  $('f-ver').textContent=S.fw||'--';
  $('f-heap').textContent=S.sys&&S.sys.heap?(S.sys.heap/1024).toFixed(0)+' KB':'--';
  $('f-up').textContent=S.sys&&S.sys.uptime?fmtUp(S.sys.uptime):'--';
  $('f-notes').textContent=RELEASE_NOTES;
  paintDisplay();
  paintPresets();
}
function renderNets(){
  let saved=(S.net&&S.net.saved)||[];
  if(!saved.length){$('nt-list').innerHTML='<div class=k>none yet</div>';return}
  $('nt-list').innerHTML=saved.map(s=>{
    let cur=s===S.net.ssid?' <span class=ok>(connected)</span>':'';
    return `<div><span class=v>${s}${cur}</span><button onclick="rmNet('${s.replace(/'/g,"\\'")}')">remove</button></div>`}).join('');
}
// Same math as server's clampLayoutForSlot — keep in sync.
function clampPageXY(p,x,y,scale){
  let w=Math.ceil(PAGE_WIDGET_DIMS[p][0]*scale/100);
  let h=Math.ceil(PAGE_WIDGET_DIMS[p][1]*scale/100);
  let minX=Math.min(0,320-w),maxX=Math.max(0,320-w);
  let minY=Math.min(0,240-h),maxY=Math.max(0,240-h);
  return {x:Math.max(minX,Math.min(maxX,x)),y:Math.max(minY,Math.min(maxY,y)),w:w,h:h};
}
function paintDisplay(){
  if(!$('dp-page')||!S.set)return;
  // Don't clobber controls while the user is actively dragging.
  if(dpDrag)return;
  let p=+$('dp-page').value||0,s=S.set,bgs=S.bgs||[];
  let cur=(s.pageBg&&s.pageBg[p])||PAGE_BG_DEFAULT[p]||'';
  let opts=bgs.slice();if(cur&&!opts.includes(cur))opts.unshift(cur);if(!opts.length)opts=[cur||'none'];
  $('dp-bg').innerHTML=opts.map(n=>`<option value="${n==='none'?'':n}">${n}</option>`).join('');
  $('dp-bg').value=cur;
  $('dp-theme').value=(s.pageTheme&&s.pageTheme[p]!=null)?s.pageTheme[p]:-1;
  $('dp-box').checked=!!(s.pageBox&&s.pageBox[p]);
  $('dp-contrast').value=(s.pageContrast&&s.pageContrast[p]!=null)?s.pageContrast[p]:0;
  let l=(s.layout&&s.layout[PAGE_LAYOUT_SLOT[p]])||PAGE_LAYOUT_DEFAULT[p];
  $('dp-s').min=PAGE_SCALE_MIN[p];$('dp-s').max=PAGE_SCALE_MAX[p];$('dp-s').value=l.scale;
  $('dp-svalue').textContent=l.scale+'%';
  let img=$('dp-img'),box=$('dp-boxel');
  if(cur){img.style.display='block';img.src='/bg/'+encodeURIComponent(cur)+'?v='+(S.bgRev||0);}else{img.style.display='none'}
  box.textContent=PAGE_NAMES[p];
  box.classList.toggle('noBox',!$('dp-box').checked && +$('dp-contrast').value<2);
  drawPreviewBox(p,l.x,l.y,l.scale);
}
// Position + size the highlighted preview rectangle using real widget dims.
function drawPreviewBox(p,x,y,scale){
  let c=clampPageXY(p,x,y,scale);
  let box=$('dp-boxel');
  box.style.left=(c.x/320*100)+'%';box.style.top=(c.y/240*100)+'%';
  box.style.width=(c.w/320*100)+'%';box.style.height=(c.h/240*100)+'%';
  if($('dp-xyvalue'))$('dp-xyvalue').textContent='X '+c.x+', Y '+c.y;
}
function dpControlChanged(field){
  let p=+$('dp-page').value||0,s=S.set=S.set||{};
  s.pageBg=s.pageBg||['','','',''];s.pageTheme=s.pageTheme||[-1,-1,-1,-1];
  s.pageBox=s.pageBox||[0,0,0,0];s.pageContrast=s.pageContrast||[0,0,0,0];
  s.layout=s.layout||[];while(s.layout.length<6)s.layout.push({x:0,y:0,scale:100});
  if(field==='bg')s.pageBg[p]=$('dp-bg').value;
  if(field==='theme')s.pageTheme[p]=+$('dp-theme').value;
  if(field==='box')s.pageBox[p]=$('dp-box').checked?1:0;
  if(field==='contrast')s.pageContrast[p]=+$('dp-contrast').value;
  if(field==='scale'){
    // Rescale changes the widget's valid bounds; keep X/Y in range so the drag
    // position doesn't visually snap when the user scales.
    let slot=PAGE_LAYOUT_SLOT[p],cur=s.layout[slot]||PAGE_LAYOUT_DEFAULT[p];
    let scale=+$('dp-s').value||100;
    let c=clampPageXY(p,cur.x,cur.y,scale);
    s.layout[slot]={x:c.x,y:c.y,scale:scale};
  }
  $('dp-svalue').textContent=($('dp-s').value||100)+'%';
  saveAll();paintDisplay();
}
// Pointer-drag to reposition. Works with mouse, touch, and pen via Pointer Events.
// The drag directly updates S.set.layout[slot] and clamps to the same X/Y range
// the server will use, so releasing never snaps.
let dpDrag=null;
function dpDragStart(e){
  e.preventDefault();
  let prev=$('dp-prev'),box=$('dp-boxel');
  let pr=prev.getBoundingClientRect();
  let p=+$('dp-page').value||0,s=S.set||{};
  let slot=PAGE_LAYOUT_SLOT[p],l=(s.layout&&s.layout[slot])||PAGE_LAYOUT_DEFAULT[p];
  dpDrag={px:e.clientX,py:e.clientY,startX:l.x,startY:l.y,scale:l.scale,page:p,slot:slot,prevW:pr.width,prevH:pr.height};
  box.classList.add('dragging');
  try{box.setPointerCapture(e.pointerId)}catch(_){}
}
function dpDragMove(e){
  if(!dpDrag)return;
  e.preventDefault();
  let dxDev=Math.round((e.clientX-dpDrag.px)*(320/dpDrag.prevW));
  let dyDev=Math.round((e.clientY-dpDrag.py)*(240/dpDrag.prevH));
  let nx=dpDrag.startX+dxDev,ny=dpDrag.startY+dyDev;
  let c=clampPageXY(dpDrag.page,nx,ny,dpDrag.scale);
  // Write directly into the working state so paint()/refresh reads the same value.
  let s=S.set=S.set||{};s.layout=s.layout||[];while(s.layout.length<6)s.layout.push({x:0,y:0,scale:100});
  s.layout[dpDrag.slot]={x:c.x,y:c.y,scale:dpDrag.scale};
  drawPreviewBox(dpDrag.page,c.x,c.y,dpDrag.scale);
}
function dpDragEnd(e){
  if(!dpDrag)return;
  let box=$('dp-boxel');
  box.classList.remove('dragging');
  try{box.releasePointerCapture(e.pointerId)}catch(_){}
  let d=dpDrag;dpDrag=null;
  // Persist. paintDisplay is skipped while dpDrag was truthy, so nothing snapped
  // mid-drag; call it explicitly so the pill text refreshes.
  saveAll();paintDisplay();
}
function resetPage(){let p=+$('dp-page').value||0;fetch('/api/reset?page='+p,{method:'POST'}).then(()=>setTimeout(load,200))}
function resetAllPages(){if(!confirm('Reset ALL display pages to defaults? (Backgrounds, themes, boxes, contrast, layout — night clock unaffected.)'))return;Promise.all([0,1,2,3].map(p=>fetch('/api/reset?page='+p,{method:'POST'}))).then(()=>setTimeout(load,300))}
function paintPresets(){
  fetch('/api/presets').then(r=>r.json()).then(d=>{
    let names=(d&&d.names)||[];
    if(!names.length){$('pre-list').innerHTML='<div class=k>none saved yet</div>';return}
    $('pre-list').innerHTML=names.map(n=>`<div><span class=v>${n}</span><span><button onclick="loadPreset('${n.replace(/'/g,"\\'")}')">Load</button> <button class="danger" onclick="delPreset('${n.replace(/'/g,"\\'")}')">&times;</button></span></div>`).join('');
  }).catch(_=>{$('pre-list').innerHTML='<div class=k>presets unavailable</div>'});
}
function savePreset(){let n=($('pre-name').value||'').trim();if(!n)return;post('/api/preset/save','name='+encodeURIComponent(n)).then(()=>{$('pre-name').value='';paintPresets()})}
function loadPreset(n){post('/api/preset/load','name='+encodeURIComponent(n)).then(()=>setTimeout(load,300))}
function delPreset(n){if(!confirm('Delete preset '+n+'?'))return;post('/api/preset/delete','name='+encodeURIComponent(n)).then(()=>setTimeout(paintPresets,200))}
function post(u,b){return fetch(u,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:b})}
function sp(d){S.th.sp=Math.max(45,Math.min(90,(S.th.sp|0)+d));$('d-set').textContent=S.th.sp;saveAll()}
function saveHost(){post('/api/hostname','name='+encodeURIComponent($('nt-host').value)).then(()=>setTimeout(load,500))}
function saveBle(){post('/api/ble','bMac='+encodeURIComponent($('b-mac').value)+'&vMac='+encodeURIComponent($('v-mac').value)+'&vKey='+encodeURIComponent($('v-key').value)+'&vPin='+encodeURIComponent($('v-pin').value)+'&gMac='+encodeURIComponent($('g-mac').value)+'&en='+($('b-en').checked?1:0)).then(()=>setTimeout(load,300))}
function clearAccent(){post('/api/settings','wAcc=').then(()=>setTimeout(load,200))}
function saveAll(){
  let s=S.set||{};
  let body=`sp=${S.th.sp||70}&mode=${$('d-mode').value}&camp=${$('d-camp').value}&theme=${$('s-theme').value}&bright=${$('s-bright').value}&anim=${$('s-anim').checked?1:0}`+
    `&nbright=${$('s-nbright').value}&autoNight=${$('s-autoNight').checked?1:0}&nStart=${$('s-nStart').value}&nEnd=${$('s-nEnd').value}&hto=${$('s-hto').value}`+
    `&doEn=${$('s-doEn').checked?1:0}&doStart=${$('s-doStart').value}&doEnd=${$('s-doEnd').value}&stMin=${$('s-stMin').value}&stMax=${$('s-stMax').value}`;
  let pbg=s.pageBg||[];for(let i=0;i<PAGE_NAMES.length;i++)body+=`&bg${i}=${encodeURIComponent(pbg[i]||'')}`;
  let pt=s.pageTheme||[],pb=s.pageBox||[],pc=s.pageContrast||[];
  for(let i=0;i<PAGE_NAMES.length;i++)body+=`&pt${i}=${pt[i]==null?-1:pt[i]}&pb${i}=${pb[i]?1:0}&pc${i}=${pc[i]||0}`;
  let ly=s.layout||[];
  for(let i=0;i<PAGE_LAYOUT_SLOT.length;i++){let slot=PAGE_LAYOUT_SLOT[i],l=ly[slot]||PAGE_LAYOUT_DEFAULT[i];body+=`&lx${slot}=${l.x}&ly${slot}=${l.y}&ls${slot}=${l.scale}`}
  post('/api/settings',body);
}
function uploadBg(e){e.preventDefault();let f=$('dp-file').files[0];if(!f)return;let fd=new FormData();fd.append('bg',f,'custom.jpg');fetch('/api/background/upload?page='+($('dp-page').value||0),{method:'POST',body:fd}).then(()=>{$('dp-file').value='';setTimeout(load,600)})}
function addNet(){let s=$('nt-ssid').value;if(!s)return;post('/api/wifi/add',`ssid=${encodeURIComponent(s)}&pass=${encodeURIComponent($('nt-pass').value)}`).then(()=>{$('nt-ssid').value='';$('nt-pass').value='';setTimeout(load,500)})}
function rmNet(s){post('/api/wifi/remove',`ssid=${encodeURIComponent(s)}`).then(()=>setTimeout(load,300))}
function pushTime(){post('/api/time','epoch='+Math.floor(Date.now()/1000))}
document.querySelectorAll('nav button').forEach(b=>b.addEventListener('click',()=>switchTab(b.dataset.tab)));
window.addEventListener('hashchange',render);
document.addEventListener('DOMContentLoaded',()=>{
  if($('wa-color'))$('wa-color').addEventListener('input',e=>post('/api/settings','wAcc='+encodeURIComponent(e.target.value)));
  let box=$('dp-boxel');
  if(box){
    box.addEventListener('pointerdown',dpDragStart);
    box.addEventListener('pointermove',dpDragMove);
    box.addEventListener('pointerup',dpDragEnd);
    box.addEventListener('pointercancel',dpDragEnd);
  }
});
window.addEventListener('resize',()=>{if(activeTab()==='power')drawVictronHistory()});
pushTime();load();loadVictronHistory();render();setInterval(load,2000);setInterval(pushTime,60000);
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
  static const uint8_t maxScale[LAYOUT_WIDGET_COUNT] = {126, 139, 181, 108, 181, 157};
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
  float peakToday = t.solPeakTodayW;
  if (!isnan(peakToday)) s["peakToday"] = peakToday;
  float monthlyPeak = victronHistoryPeakPowerW();
  if (!isnan(monthlyPeak)) s["monthPeak"] = monthlyPeak;
  if (!isnan(t.solPvW) && !isnan(monthlyPeak) && monthlyPeak > 0.5f) {
    s["monthPct"] = min(100.0f, max(0.0f, t.solPvW * 100.0f / monthlyPeak));
  }
  if (!isnan(t.solBattV)) s["v"] = t.solBattV;
  if (!isnan(t.solBattA)) s["a"] = t.solBattA;
  if (!isnan(t.solLoadA)) s["loadA"] = t.solLoadA;
  if (!isnan(t.solYieldKwh)) s["yield"] = t.solYieldKwh;
  if (!isnan(t.solPvV)) s["pvV"] = t.solPvV;
  if (!isnan(t.solLoadV)) s["loadV"] = t.solLoadV;
  if (t.solLoadState >= 0) s["loadOn"] = t.solLoadState != 0;
  if (!isnan(t.solTotalYieldKwh)) s["totalYield"] = t.solTotalYieldKwh;
  if (!isnan(t.solUserYieldKwh)) s["userYield"] = t.solUserYieldKwh;
  if (!isnan(t.solBattMinTodayV)) s["bMin"] = t.solBattMinTodayV;
  if (!isnan(t.solBattMaxTodayV)) s["bMax"] = t.solBattMaxTodayV;
  if (!isnan(t.solPvMaxTodayV)) s["pvMaxV"] = t.solPvMaxTodayV;
  if (!isnan(t.solMaxBattCurrentTodayA)) s["maxBattA"] = t.solMaxBattCurrentTodayA;
  s["bulkMin"] = t.solBulkMinutesToday;
  s["absMin"] = t.solAbsorptionMinutesToday;
  s["floatMin"] = t.solFloatMinutesToday;
  s["historyDays"] = t.solHistoryDays;
  if (t.solProductId) s["productId"] = t.solProductId;
  if (t.solModel[0]) s["model"] = t.solModel;
  if (t.solSerial[0]) s["serial"] = t.solSerial;
  if (t.solFwVersion) s["fw"] = t.solFwVersion;
  if (!isnan(t.solBattTempC)) s["battTemp"] = t.solBattTempC;
  if (!isnan(t.solYieldYesterdayKwh)) s["yieldYest"] = t.solYieldYesterdayKwh;
  if (!isnan(t.solMaxPowerYesterdayW)) s["peakYest"] = t.solMaxPowerYesterdayW;
  if (t.solUnknownVregCount) {
    auto uv = s["unknownVregs"].to<JsonArray>();
    for (uint8_t i = 0; i < t.solUnknownVregCount; i++) uv.add(t.solUnknownVregs[i]);
  }
  if (t.solConnectedLastMs) s["connectedAge"] = (uint32_t)(millis() - t.solConnectedLastMs) / 1000;
  s["rssi"] = t.solRssi;
  auto th = d["th"].to<JsonObject>();
  th["sp"] = gSettings.setpointF; th["mode"] = gSettings.mode; th["camp"] = gSettings.camping;
  if (!isnan(t.insideTempF)) th["inside"] = t.insideTempF;
  auto n = d["net"].to<JsonObject>();
  n["sta"] = gNet.staConnected; n["ssid"] = gNet.ssid; n["ip"] = gNet.ip;
  n["ap"] = gNet.apActive ? gNet.apSsid : String("");
  n["tsrc"] = gNet.timeSource;
  n["host"] = gSettings.hostname;
  auto saved = n["saved"].to<JsonArray>();
  for (auto& w : gSettings.networks) saved.add(w.ssid);
  auto st = d["set"].to<JsonObject>();
  st["theme"] = gSettings.dayThemeIdx; st["bright"] = gSettings.dayBrightness; st["anim"] = gSettings.animations;
  st["nbright"] = gSettings.nightBrightness; st["autoNight"] = gSettings.autoNight;
  st["nStart"] = gSettings.nightStartHour; st["nEnd"] = gSettings.nightEndHour;
  st["hto"] = gSettings.homeTimeoutSec;
  st["doEn"] = gSettings.dispOffEnable; st["doStart"] = gSettings.dispOffStartHour; st["doEnd"] = gSettings.dispOffEndHour;
  st["stMin"] = gSettings.storeMinF; st["stMax"] = gSettings.storeMaxF;
  st["wAcc"] = gSettings.webAccent;
  st["dayBg"] = gSettings.pageBg[PAGE_CLOCK];
  auto pageBg = st["pageBg"].to<JsonArray>();
  for (size_t i = 0; i < gSettings.pageBg.size(); i++) pageBg.add(gSettings.pageBg[i]);
  auto pageTheme = st["pageTheme"].to<JsonArray>();
  for (size_t i = 0; i < gSettings.pageTheme.size(); i++) pageTheme.add(gSettings.pageTheme[i]);
  auto pageBox = st["pageBox"].to<JsonArray>();
  for (size_t i = 0; i < gSettings.pageBox.size(); i++) pageBox.add(gSettings.pageBox[i] ? 1 : 0);
  auto pageContrast = st["pageContrast"].to<JsonArray>();
  for (size_t i = 0; i < gSettings.pageContrast.size(); i++) pageContrast.add(gSettings.pageContrast[i]);
  auto layout = st["layout"].to<JsonArray>();
  for (size_t i = 0; i < gSettings.layout.size(); i++) {
    JsonObject o = layout.add<JsonObject>();
    o["x"] = gSettings.layout[i].x;
    o["y"] = gSettings.layout[i].y;
    o["scale"] = gSettings.layout[i].scale;
  }
  auto ble = d["ble"].to<JsonObject>();
  ble["bMac"] = gSettings.batteryMac;
  ble["vMac"] = gSettings.victronMac;
  ble["vKey"] = gSettings.victronKey;
  ble["vPinSet"] = isVictronPin(gSettings.victronPin);
  ble["gMac"] = gSettings.gidroxMac;
  ble["en"] = gSettings.bleEnabled;
  auto themes = d["themes"].to<JsonArray>();
  for (size_t i = 0; i < HUCK_THEME_COUNT; i++) themes.add(HUCK_THEMES[i].name);
  auto bgs = d["bgs"].to<JsonArray>();
  addBackgrounds(bgs);
  d["bgRev"] = s_bgRev;
  d["fw"] = FW_VERSION;
  auto sys = d["sys"].to<JsonObject>();
  sys["heap"] = (uint32_t)ESP.getFreeHeap();
  sys["uptime"] = (uint32_t)(millis() / 1000);
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

static void appendJsonFloat(String& output, const char* key, float value) {
  if (isnan(value)) return;
  output += ",\"";
  output += key;
  output += "\":";
  output += String(value, 3);
}

static void handleVictronHistory() {
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", "");
  server.sendContent("{\"days\":[");
  bool first = true;
  for (size_t i = 0; i < HUCK_VICTRON_HISTORY_DAYS; i++) {
    VictronDay day;
    if (!victronDayCopy(i, day)) continue;
    if (!day.valid) continue;
    String output;
    output.reserve(256);
    if (!first) output += ',';
    first = false;
    output += "{\"age\":";
    output += day.ageDays;
    output += ",\"seq\":";
    output += day.sequence;
    appendJsonFloat(output, "yield", day.yieldKwh);
    appendJsonFloat(output, "consumed", day.consumedKwh);
    appendJsonFloat(output, "bmax", day.battMaxV);
    appendJsonFloat(output, "bmin", day.battMinV);
    appendJsonFloat(output, "peak", day.peakPowerW);
    appendJsonFloat(output, "imax", day.maxBattCurrentA);
    appendJsonFloat(output, "pvmax", day.pvMaxV);
    output += ",\"bulk\":";
    output += day.bulkMinutes;
    output += ",\"abs\":";
    output += day.absorptionMinutes;
    output += ",\"float\":";
    output += day.floatMinutes;
    output += ",\"errors\":[";
    for (size_t errorIndex = 0; errorIndex < 4; errorIndex++) {
      if (errorIndex) output += ',';
      output += static_cast<unsigned int>(day.errors[errorIndex]);
    }
    output += ']';
    output += '}';
    server.sendContent(output);
  }
  server.sendContent("]}");
  server.sendContent("");
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
  if (server.hasArg("wAcc")) {
    String a = server.arg("wAcc");
    if (isSafeAccent(a)) gSettings.webAccent = a;
  }
  auto setBg = [](size_t idx, const String& bg) {
    if (idx >= gSettings.pageBg.size()) return;
    if (bg.isEmpty() || (isSafeBgName(bg) && (!s_fsOk || SPIFFS.exists(bgPath(bg))))) {
      if (gSettings.pageBg[idx] != bg) gBgReloadRequested = true;
      gSettings.pageBg[idx] = bg;
    }
  };
  if (server.hasArg("bg")) setBg(PAGE_CLOCK, server.arg("bg"));
  for (size_t i = 0; i < gSettings.pageBg.size(); i++) {
    char kb[8], kt[8], kbox[8], kc[8];
    snprintf(kb, sizeof(kb), "bg%u", (unsigned)i);
    snprintf(kt, sizeof(kt), "pt%u", (unsigned)i);
    snprintf(kbox, sizeof(kbox), "pb%u", (unsigned)i);
    snprintf(kc, sizeof(kc), "pc%u", (unsigned)i);
    if (server.hasArg(kb)) setBg(i, server.arg(kb));
    if (server.hasArg(kt)) {
      gSettings.pageTheme[i] = (int8_t)clampWebInt(server.arg(kt).toInt(), -1, (int)HUCK_THEME_COUNT - 1);
    }
    if (server.hasArg(kbox)) gSettings.pageBox[i] = server.arg(kbox).toInt() != 0;
    if (server.hasArg(kc)) gSettings.pageContrast[i] = (uint8_t)clampWebInt(server.arg(kc).toInt(), 0, 2);
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

static void handleReset() {
  int page = server.hasArg("page") ? clampWebInt(server.arg("page").toInt(), 0, PAGE_COUNT - 1) : -1;
  if (page < 0) { server.send(400, "text/plain", "page required"); return; }
  gSettings.pageBg[page] = PAGE_BG_DEFAULT[page];
  gSettings.pageTheme[page] = PAGE_THEME_DEFAULT[page];
  gSettings.pageBox[page] = PAGE_BOX_DEFAULT[page];
  gSettings.pageContrast[page] = PAGE_CONTRAST_DEFAULT[page];
  int slot = PAGE_LAYOUT_SLOT[page];
  gSettings.layout[slot] = clampLayoutForSlot((LayoutWidget)slot, LAYOUT_DEFAULT[slot]);
  // The day-date label lives on the clock page and gets reset alongside it.
  if (page == PAGE_CLOCK) {
    gSettings.layout[LAYOUT_DAY_DATE] = clampLayoutForSlot(LAYOUT_DAY_DATE, LAYOUT_DEFAULT[LAYOUT_DAY_DATE]);
  }
  if (page == PAGE_POWER) {
    // Power page has a second widget (stats card). Reset both.
    gSettings.layout[LAYOUT_POWER_STATS] = clampLayoutForSlot(LAYOUT_POWER_STATS, LAYOUT_DEFAULT[LAYOUT_POWER_STATS]);
  }
  gSettings.save();
  gBgReloadRequested = true;
  gUiApplyRequested = true;
  server.send(200, "text/plain", "ok");
}

// ---- Seasonal presets (SPIFFS /presets/<name>.json) ----
static void handlePresetsList() {
  JsonDocument d;
  auto names = d["names"].to<JsonArray>();
  listPresets(names);
  String out; serializeJson(d, out);
  server.send(200, "application/json", out);
}

static void handlePresetSave() {
  if (!s_fsOk) { server.send(500, "text/plain", "no filesystem"); return; }
  String name = server.arg("name");
  if (!isSafePresetName(name)) { server.send(400, "text/plain", "bad name"); return; }
  SPIFFS.mkdir("/presets");
  JsonDocument d;
  d["name"] = name;
  d["dayTheme"] = gSettings.dayThemeIdx;
  auto pbg = d["pageBg"].to<JsonArray>();
  for (auto& s : gSettings.pageBg) pbg.add(s);
  auto pt = d["pageTheme"].to<JsonArray>();
  for (auto v : gSettings.pageTheme) pt.add(v);
  auto pb = d["pageBox"].to<JsonArray>();
  for (auto v : gSettings.pageBox) pb.add(v ? 1 : 0);
  auto pc = d["pageContrast"].to<JsonArray>();
  for (auto v : gSettings.pageContrast) pc.add(v);
  auto ly = d["layout"].to<JsonArray>();
  for (auto& l : gSettings.layout) { JsonObject o = ly.add<JsonObject>(); o["x"] = l.x; o["y"] = l.y; o["scale"] = l.scale; }
  File f = SPIFFS.open(presetPath(name), "w");
  if (!f) { server.send(500, "text/plain", "open failed"); return; }
  serializeJson(d, f);
  f.close();
  server.send(200, "text/plain", "ok");
}

static void handlePresetLoad() {
  if (!s_fsOk) { server.send(500, "text/plain", "no filesystem"); return; }
  String name = server.arg("name");
  if (!isSafePresetName(name)) { server.send(400, "text/plain", "bad name"); return; }
  File f = SPIFFS.open(presetPath(name), "r");
  if (!f) { server.send(404, "text/plain", "not found"); return; }
  JsonDocument d;
  DeserializationError err = deserializeJson(d, f);
  f.close();
  if (err) { server.send(400, "text/plain", "bad preset"); return; }
  if (d["dayTheme"].is<int>()) {
    gSettings.dayThemeIdx = clampWebInt(d["dayTheme"].as<int>(), 0, (int)HUCK_THEME_COUNT - 1);
  }
  JsonArray pbg = d["pageBg"];
  JsonArray pt  = d["pageTheme"];
  JsonArray pb  = d["pageBox"];
  JsonArray pc  = d["pageContrast"];
  JsonArray ly  = d["layout"];
  for (size_t i = 0; i < gSettings.pageBg.size(); i++) {
    if (i < pbg.size()) {
      String bg = pbg[i].as<String>();
      if (bg.isEmpty() || (isSafeBgName(bg) && SPIFFS.exists(bgPath(bg)))) gSettings.pageBg[i] = bg;
    }
    if (i < pt.size()) gSettings.pageTheme[i] = (int8_t)clampWebInt(pt[i].as<int>(), -1, (int)HUCK_THEME_COUNT - 1);
    if (i < pb.size()) gSettings.pageBox[i] = pb[i].as<int>() != 0;
    if (i < pc.size()) gSettings.pageContrast[i] = (uint8_t)clampWebInt(pc[i].as<int>(), 0, 2);
  }
  for (size_t i = 0; i < gSettings.layout.size() && i < ly.size(); i++) {
    JsonObject o = ly[i];
    LayoutSlot slot = { (int16_t)o["x"].as<int>(), (int16_t)o["y"].as<int>(), (uint8_t)o["scale"].as<int>() };
    gSettings.layout[i] = clampLayoutForSlot((LayoutWidget)i, slot);
  }
  gSettings.save();
  gBgReloadRequested = true;
  gUiApplyRequested = true;
  server.send(200, "text/plain", "ok");
}

static void handlePresetDelete() {
  if (!s_fsOk) { server.send(500, "text/plain", "no filesystem"); return; }
  String name = server.arg("name");
  if (!isSafePresetName(name)) { server.send(400, "text/plain", "bad name"); return; }
  if (SPIFFS.exists(presetPath(name))) SPIFFS.remove(presetPath(name));
  server.send(200, "text/plain", "ok");
}

static void handleHostname() {
  String h = server.arg("name");
  if (!isSafeHostname(h)) { server.send(400, "text/plain", "bad hostname"); return; }
  gSettings.hostname = h;
  gSettings.save();
  // Full effect (WiFi/mDNS advert) applies after reboot; the value is live now.
  server.send(200, "text/plain", "ok");
}

static void handleBle() {
  if (server.hasArg("bMac")) gSettings.batteryMac = server.arg("bMac");
  if (server.hasArg("vMac")) gSettings.victronMac = server.arg("vMac");
  if (server.hasArg("vKey")) gSettings.victronKey = server.arg("vKey");
  if (server.hasArg("vPin") && !server.arg("vPin").isEmpty()) {
    if (!isVictronPin(server.arg("vPin"))) {
      server.send(400, "text/plain", "Victron PIN must be exactly 6 digits");
      return;
    }
    gSettings.victronPin = server.arg("vPin");
  }
  if (server.hasArg("gMac")) gSettings.gidroxMac = server.arg("gMac");
  if (server.hasArg("en"))   gSettings.bleEnabled = server.arg("en").toInt() != 0;
  gSettings.save();
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
<p>Current: <b>)HTML" FW_VERSION R"HTML(</b>. Upload <code>.pio/build/huckleberry/firmware.bin</code>.</p>
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
  if (s_fsOk) { SPIFFS.mkdir("/bg"); SPIFFS.mkdir("/presets"); }

  server.on("/", HTTP_GET, [] { server.send_P(200, "text/html", PAGE); });
  server.on("/api/state", HTTP_GET, handleState);
  server.on("/api/victron/history", HTTP_GET, handleVictronHistory);
  server.on("/api/settings", HTTP_POST, handleSettings);
  server.on("/api/reset", HTTP_POST, handleReset);
  server.on("/api/presets", HTTP_GET, handlePresetsList);
  server.on("/api/preset/save", HTTP_POST, handlePresetSave);
  server.on("/api/preset/load", HTTP_POST, handlePresetLoad);
  server.on("/api/preset/delete", HTTP_POST, handlePresetDelete);
  server.on("/api/hostname", HTTP_POST, handleHostname);
  server.on("/api/ble", HTTP_POST, handleBle);
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
  server.onNotFound([] {
    if (server.uri().startsWith("/bg/")) { handleBgFile(); return; }
    server.sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/");
    server.send(302, "text/plain", "");
  });
  server.begin();
}

void loop() { server.handleClient(); }

} // namespace web
