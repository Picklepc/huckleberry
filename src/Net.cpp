#include "Net.h"
#include "Settings.h"
#include "AppState.h"

#include <WiFi.h>
#include <ESPmDNS.h>
#include <DNSServer.h>
#include <time.h>
#include <sys/time.h>

namespace net {

static DNSServer  s_dns;
static bool       s_dnsUp = false;
static bool       s_mdnsUp = false;
static uint32_t   s_lastStaTry = 0;
static bool       s_ntpConfigured = false;
static const uint32_t STA_RETRY_MS = 20000;

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
  bool haveCreds = !gSettings.wifiSsid.isEmpty();
  WiFi.mode(haveCreds ? WIFI_AP_STA : WIFI_AP);   // AP always up for off-grid
  startAp();
  if (haveCreds) {
    WiFi.begin(gSettings.wifiSsid.c_str(), gSettings.wifiPass.c_str());
    s_lastStaTry = millis();
  }
  // NTP (takes effect once STA has internet); TZ set for local wall time.
  configTzTime(gSettings.tz.c_str(), "pool.ntp.org", "time.nist.gov");
  s_ntpConfigured = true;
}

void loop() {
  if (s_dnsUp) s_dns.processNextRequest();

  bool sta = WiFi.status() == WL_CONNECTED;
  gNet.staConnected = sta;
  if (sta) {
    gNet.ssid = gSettings.wifiSsid;
    gNet.ip = WiFi.localIP().toString();
    if (!s_mdnsUp) {
      if (MDNS.begin(gSettings.hostname.c_str())) { MDNS.addService("http", "tcp", 80); s_mdnsUp = true; }
    }
    // NTP may have set the clock — mark source unless a browser already did.
    if (timeIsValid() && gNet.timeSource == "build") { gNet.timeSynced = true; gNet.timeSource = "ntp"; }
  } else {
    gNet.ip = gNet.apActive ? WiFi.softAPIP().toString() : "";
    if (!gSettings.wifiSsid.isEmpty() && millis() - s_lastStaTry > STA_RETRY_MS) {
      s_lastStaTry = millis();
      WiFi.begin(gSettings.wifiSsid.c_str(), gSettings.wifiPass.c_str());
    }
  }
}

} // namespace net
