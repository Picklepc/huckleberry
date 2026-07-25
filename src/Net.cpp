#include "Net.h"
#include "Settings.h"
#include "AppState.h"

#include <WiFi.h>
#include <ESPmDNS.h>
#include <DNSServer.h>
#include <ArduinoOTA.h>
#include <time.h>
#include <sys/time.h>

namespace net {

static DNSServer  s_dns;
static bool       s_dnsUp = false;
static bool       s_mdnsUp = false;
static bool       s_otaUp = false;
static bool       s_ntpConfigured = false;
static const uint32_t ATTEMPT_MS = 9000;   // per-network join timeout
static int        s_curIdx = -1;           // network we're currently trying
static uint32_t   s_attemptStart = 0;

static void tryNetwork(int idx) {
  if (idx < 0 || (size_t)idx >= gSettings.networks.size()) return;
  s_curIdx = idx;
  s_attemptStart = millis();
  const auto& n = gSettings.networks[idx];
  WiFi.begin(n.ssid.c_str(), n.pass.c_str());
}

bool timeIsValid() { return time(nullptr) > 1700000000; }

void setTimeFromEpoch(uint32_t epoch, const char* source) {
  if (epoch < 1700000000) return;
  struct timeval tv = { .tv_sec = (time_t)epoch, .tv_usec = 0 };
  settimeofday(&tv, nullptr);
  gNet.timeSynced = true;
  gNet.timeSource = source;
}

static void startAp() {
  WiFi.softAP(gSettings.apSsid.c_str(),
              gSettings.apPass.isEmpty() ? nullptr : gSettings.apPass.c_str(),
              6, false, 4);
  gNet.apActive = true;
  gNet.apSsid = gSettings.apSsid;
  if (!s_dnsUp) {
    s_dns.setErrorReplyCode(DNSReplyCode::NoError);
    s_dnsUp = s_dns.start(53, "*", WiFi.softAPIP());
  }
}

void begin() {
  WiFi.persistent(false);
  WiFi.setHostname(gSettings.hostname.c_str());
  WiFi.mode(gSettings.hasNetworks() ? WIFI_AP_STA : WIFI_AP);  // AP always up for off-grid
  startAp();
  if (gSettings.hasNetworks()) tryNetwork(0);
  // NTP (takes effect once STA has internet); TZ set for local wall time.
  configTzTime(gSettings.tz.c_str(), "pool.ntp.org", "time.nist.gov");
  s_ntpConfigured = true;
}

// Call after the saved-network list changes, to restart the join sequence.
void reconnect() {
  if (gSettings.hasNetworks()) {
    if (WiFi.getMode() == WIFI_AP) WiFi.mode(WIFI_AP_STA);
    tryNetwork(0);
  }
}

void loop() {
  if (s_dnsUp) s_dns.processNextRequest();

  bool sta = WiFi.status() == WL_CONNECTED;
  gNet.staConnected = sta;
  if (sta) {
    gNet.ssid = WiFi.SSID();
    gNet.ip = WiFi.localIP().toString();
    if (!s_mdnsUp) {
      if (MDNS.begin(gSettings.hostname.c_str())) { MDNS.addService("http", "tcp", 80); s_mdnsUp = true; }
    }
    if (!s_otaUp) {   // PlatformIO/espota over Wi-Fi from any dev machine
      ArduinoOTA.setHostname(gSettings.hostname.c_str());
      ArduinoOTA.setPassword("huckleberry");
      ArduinoOTA.begin();
      s_otaUp = true;
    }
    ArduinoOTA.handle();
    if (timeIsValid() && gNet.timeSource == "build") { gNet.timeSynced = true; gNet.timeSource = "ntp"; }
  } else {
    gNet.ssid = "";
    gNet.ip = gNet.apActive ? WiFi.softAPIP().toString() : "";
    if (s_mdnsUp) { MDNS.end(); s_mdnsUp = false; }
    // Cycle through saved networks: each gets ATTEMPT_MS to associate.
    size_t n = gSettings.networks.size();
    if (n && millis() - s_attemptStart > ATTEMPT_MS) {
      tryNetwork((s_curIdx + 1) % (int)n);
    }
  }
}

} // namespace net
