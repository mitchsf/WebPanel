// Render the WordClock-5 4.4.11 and WordClock-1-4 3.7.11 one-page setup forms
// through the real WebPanel code and report the page size WebPanel measured.
#include <Arduino.h>
#include <WiFi.h>
#include <WebPanel.h>
#include <cstdio>
#include <new>
#include <cctype>

unsigned long g_millis = 0;
EspClass ESP;
WiFiClass WiFi;
HostCapture g_http;

static const char* OPT_MONTHS = "January, February, March, April, May, June, July, August, September, October, November, December";
static const char* OPT_DAYS   = "Sunday, Monday, Tuesday, Wednesday, Thursday, Friday, Saturday";

// Representative scan: 12 networks with long names.
static String ssidCsv() {
  String s;
  const char* names[] = {"Greyhound-5G", "NETGEAR-Guest-Extended", "xfinitywifi", "MyHome Network 2.4",
                         "TP-Link_8A2C_5G", "Spectrum-Setup-77", "BrightHouse-Mesh", "Linksys09876",
                         "ATTxyz1234_5GHz", "Neighbors-IoT", "Verizon_ABC123", "HP-Print-55-Officejet"};
  for (int i = 0; i < 12; i++) { if (i) s += ", "; s += names[i]; }
  return s;
}

// Globals bound by the forms (values typical of a configured unit).
static int preset[64];
static int ssidIndex = 0;
static String xApName = "WordClock-5-2", xSSIDOverride = "", xPassword = "hunter2hunter2", xLiveFormPassword = "",
              xNtpServ = "us.pool.ntp.org", xNtpServ2 = "time.google.com", xOtaUrl = "https://mitchsf.github.io/Firmware/";
static int cfgNtpPollIndex = 5, cfgLiveForm = 1, cfgUtcOffset = 500, cfgUtcDirection = 0, cfgDstStartMonth = 2,
           cfgDstStartWeek = 2, cfgDstStartDay = 0, cfgDstStartHour = 2, cfgDstOffset = 1, cfgDstDirection = 1,
           cfgStdStartMonth = 10, cfgStdStartWeek = 1, cfgStdStartDay = 0, cfgStdStartHour = 2, cfgTimeDisplay = 0,
           cfgDateDisplay = 0, cfgTimeDateLang = 0, libSelect[5] = {0, 0, 0, 0, 0}, cfgUseLib[4] = {2, 0, 0, 0};
static String xKey = "", xpCode = "", xpCountry = "US";

static void wc5Setup(WebPanel& f) {
  f.addSubheading("Network");
  f.addText("Access Point Name", "ap", &xApName);
  f.addDropDown("SSID", "ssid", ssidCsv(), &ssidIndex,
                "The network shown here is the one that gets saved. "
                "'not in range' means the saved network isn't "
                "currently broadcasting.");
  f.addText("SSID Override", "ssidOver", &xSSIDOverride, "",
            "Takes priority over the SSID list above whenever it is "
            "filled in. Leave blank unless the network is hidden.");
  f.addPassword("Network Password", "pw", &xPassword);
  f.addPassword("Live Form Password", "lfpw", &xLiveFormPassword, "Leave blank to disable");
  f.addText("NTP Server", "ntp", &xNtpServ, "Primary time server");
  f.addText("Alternate NTP Server", "ntp2", &xNtpServ2, "Used if primary fails; leave blank to disable");
  f.addDropDown("NTP Polling Interval (minutes)", "ntpPoll", "5, 15, 30, 60, 120, 240", &preset[49],
                "How often to re-sync the clock from the NTP server. "
                "Does not affect boot or retry timing.");
  f.addToggle("Live Form", "liveform", &preset[44],
    "Serve the settings web page while the clock is running. Display, announcement and date "
    "display settings are only on that page. Off = no web interface in run mode; hold SW2 to "
    "reach this setup form and turn it back on.");
  f.addSubheading("Date");
  f.addDropDown("Date Format", "dateformat", "MM/DD/YY, DD/MM/YY, YY/MM/DD", &preset[46]);
  f.addSubheading("UTC Offset");
  f.addHourMinute("UTC Offset", "utc", &preset[29], "0, 30, 45");
  f.addRadio("UTC Direction", "utcdir", "-, +", &preset[30]);
  f.addSubheading("Daylight Saving Time");
  f.addDropDown("DST Start Month", "dstm", OPT_MONTHS, &preset[31]);
  f.addDropDownRange("DST Start Week", "dstw", 1, 5, &preset[32]);
  f.addDropDown("DST Start Day", "dstd", OPT_DAYS, &preset[33]);
  f.addDropDownRange("DST Start Hour", "dsth", 0, 23, &preset[34]);
  f.addDropDownRange("DST Offset", "dsto", 0, 1, &preset[35]);
  f.addRadio("DST Direction", "dstdir", "-, +", &preset[36]);
  f.addSubheading("Standard Time");
  f.addDropDown("ST Start Month", "stdm", OPT_MONTHS, &preset[37]);
  f.addDropDownRange("ST Start Week", "stdw", 1, 5, &preset[38]);
  f.addDropDown("ST Start Day", "stdd", OPT_DAYS, &preset[39]);
  f.addDropDownRange("ST Start Hour", "stdh", 0, 23, &preset[40]);
  f.addSubheading("OTA Update");
  f.addText("Firmware URL", "ota", &xOtaUrl);
  f.hidePageSaveButton(-1);
  f.addActionButton("Start", "start", "✓ Starting...");
}

static void wc14Setup(WebPanel& f, int nLibs) {
  f.addSubheading("Network");
  f.addText("Access Point Name", "ap", &xApName);
  f.addDropDown("SSID", "ssid", ssidCsv(), &ssidIndex,
                "The network shown here is the one that gets saved. "
                "'not in range' means the saved network isn't "
                "currently broadcasting.");
  f.addText("SSID Override", "ssidOver", &xSSIDOverride, "",
            "Takes priority over the SSID list above whenever it is "
            "filled in. Leave blank unless the network is hidden.");
  f.addPassword("Network Password", "pw", &xPassword);
  f.addPassword("Live Form Password", "lfpw", &xLiveFormPassword, "Leave blank to disable");
  f.addText("NTP Server", "ntp", &xNtpServ, "Primary time server");
  f.addText("Alternate NTP Server", "ntp2", &xNtpServ2, "Used if primary fails; leave blank to disable");
  f.addDropDown("NTP Polling Interval (minutes)", "ntpPoll", "5, 15, 30, 60, 120, 240", &cfgNtpPollIndex,
                "How often to re-sync from the upstream NTP server. "
                "Does not affect boot or retry timing.");
  f.addToggle("Live Form", "liveform", &cfgLiveForm);
  f.addSubheading("UTC Offset");
  f.addHourMinute("UTC Offset", "utc", &cfgUtcOffset, "0, 30, 45");
  f.addDropDown("UTC Direction", "utcdir", "-, +", &cfgUtcDirection);
  f.addSubheading("Daylight Saving Time");
  f.addDropDown("DST Start Month", "dstm", OPT_MONTHS, &cfgDstStartMonth);
  f.addDropDownRange("DST Start Week", "dstw", 1, 5, &cfgDstStartWeek);
  f.addDropDown("DST Start Day", "dstd", OPT_DAYS, &cfgDstStartDay);
  f.addDropDownRange("DST Start Hour", "dsth", 0, 23, &cfgDstStartHour);
  f.addDropDownRange("DST Offset", "dsto", 0, 1, &cfgDstOffset, "Hours added during DST");
  f.addDropDown("DST Direction", "dstdir", "-, +", &cfgDstDirection);
  f.addSubheading("Standard Time");
  f.addDropDown("ST Start Month", "stdm", OPT_MONTHS, &cfgStdStartMonth);
  f.addDropDownRange("ST Start Week", "stdw", 1, 5, &cfgStdStartWeek);
  f.addDropDown("ST Start Day", "stdd", OPT_DAYS, &cfgStdStartDay);
  f.addDropDownRange("ST Start Hour", "stdh", 0, 23, &cfgStdStartHour);
  f.addSubheading("Time");
  f.addDropDown("Time Display", "tdisp", "12 Hour, 24 Hour, US English Verbose", &cfgTimeDisplay);
  f.addDropDown("Date Display", "ddisp", "MM/DD/YY, DD/MM/YY, YY/MM/DD, US English Verbose, Off", &cfgDateDisplay);
  f.addSubheading("Libraries");
  const char* folders[] = {"English", "Deutsch", "Francais", "Espanol", "Italiano", "Nederlands",
                           "Portugues", "Svenska", "Dansk", "Norsk", "Suomi", "Polski"};
  for (byte lNo = 0; lNo < 5; lNo++) {
    String libOptions = "";
    int optionIndex = 0;
    if (lNo > 0) { libOptions = "-"; optionIndex = 1; }
    for (int x = 0; x < nLibs; x++) {
      if (optionIndex > 0) libOptions += ", ";
      libOptions += folders[x];
      optionIndex++;
    }
    f.addDropDown("Library " + String(lNo + 1), "lib" + String(lNo), libOptions, &libSelect[lNo],
                  lNo == 0 ? "Primary library (required)" : "");
    if (lNo >= 1 && lNo <= 4)
      f.addDropDown("Use Library " + String(lNo + 1), "uselib" + String(lNo + 1), "Off, 5%, 20%", &cfgUseLib[lNo - 1]);
  }
  f.addDropDown("Time and Date Language", "tdlang", "Always Library 1, Current Library", &cfgTimeDateLang);
  f.addSubheading("Outside Weather");
  f.addText("Weather Key", "wxkey", &xKey, "Visual Crossing API key");
  f.addText("Postal Code or Lat/Lon", "wxzip", &xpCode);
  f.addText("Country Code (2 Letter)", "wxcc", &xpCountry);
  f.addSubheading("OTA Update");
  f.addText("Firmware URL", "ota", &xOtaUrl, "Server URL for OTA updates");
  f.hidePageSaveButton(-1);
  f.addActionButton("Start", "start", "✓ Starting...");
}

static void render(const char* title, int bufBytes, void (*build)(WebPanel&, int), int arg, const char* path = "/") {
  WebPanel::freeBuffer();
  WebPanel::setBufferSize(bufBytes);
  WebPanel::allocBuffer();
  WiFiServer server(80);
  // Zero the storage first, like a global on the ESP32 — a stack WebPanel left
  // uninitialised members as garbage and made renders differ run to run.
  alignas(WebPanel) static unsigned char storage[sizeof(WebPanel)];
  memset(storage, 0, sizeof storage);
  WebPanel& panel = *new (storage) WebPanel();
  panel.setTitle(title, "v0.0.0");
  panel.setRebootOnSave(false);
  panel.setMaxFields(60);
  panel.begin(&server);
  build(panel, arg);
  g_http = HostCapture();
  g_http.request = std::string("GET ") + path + " HTTP/1.1\r\nHost: 192.168.4.1\r\n\r\n";
  g_http.pending = true;
  panel.handleClient();
  printf("%-44s buffer %6d  page %6d bytes  %s  (response %zu B)\n", title, bufBytes,
         WebPanel::lastRenderLen(), WebPanel::lastRenderTruncated() ? "TRUNCATED" : "ok", g_http.response.size());
  // Dump the response body (after the blank line) for before/after diffs.
  std::string fn = std::string(getenv("WPHOST_OUT") ? getenv("WPHOST_OUT") : "out") + "/";
  for (const char* c = title; *c; ++c) fn += (isalnum((unsigned char)*c) ? *c : '_');
  fn += ".html";
  size_t hdrEnd = g_http.response.find("\r\n\r\n");
  if (FILE* fp = fopen(fn.c_str(), "wb")) {
    if (hdrEnd != std::string::npos) fwrite(g_http.response.data() + hdrEnd + 4, 1, g_http.response.size() - hdrEnd - 4, fp);
    fclose(fp);
  }
  // Also fetch the static assets if the library serves them (ignored on 1.3.x).
  for (const char* asset : {"/wp.css", "/wp.js"}) {
    g_http = HostCapture();
    g_http.request = std::string("GET ") + asset + "?v=x HTTP/1.1\r\nHost: 192.168.4.1\r\n\r\n";
    g_http.pending = true;
    panel.handleClient();
    hdrEnd = g_http.response.find("\r\n\r\n");
    if (g_http.response.rfind("HTTP/1.1 200", 0) == 0 && hdrEnd != std::string::npos) {
      std::string an = std::string(getenv("WPHOST_OUT") ? getenv("WPHOST_OUT") : "out") + asset;
      if (FILE* fp = fopen(an.c_str(), "wb")) { fwrite(g_http.response.data() + hdrEnd + 4, 1, g_http.response.size() - hdrEnd - 4, fp); fclose(fp); }
      printf("    %s: %zu bytes, headers: %s\n", asset, g_http.response.size() - hdrEnd - 4,
             g_http.response.substr(0, hdrEnd).substr(g_http.response.find("\r\n") + 2).c_str());
    }
  }
}

static void wc5(WebPanel& f, int) { wc5Setup(f); }
static void chromeOnly(WebPanel& f, int) { f.hidePageSaveButton(-1); f.addActionButton("Start", "start", "✓ Starting..."); }

// Old WordClock-1-4 3.7.10 layout: the Setup sub-page (page 0) and the
// Information sub-page (page 3, the largest) — both known to render on
// no-PSRAM units in the field, so they calibrate the host numbers.
static int cfgInfoSec = 10, cfgLightSensor = 0, cfgLightMinBr = 3, cfgLightMinLed = 3, cfgPir = 0, cfgWxSec = 35,
           cfgWxInt = 0, cfgWxScale = 0, cfgPressureUnit = 0, cfgInsideTemp = 0, cfgTempOffset = 0, cfgInsidePress = 0,
           cfgInsideHumid = 0, cfgOutTemp = 0, cfgOutPress = 0, cfgOutHumid = 0, cfgOutWind = 0, cfgCurrentWx = 0,
           cfgWxLabels = 0, cfgWxTimeSlice = 0;
static void wc14Old(WebPanel& f, int) {
  f.addPage("WordClock-2-6 Settings", "Setup");
  f.addSubheading("Network");
  f.addText("Access Point Name", "ap", &xApName);
  f.addDropDown("SSID", "ssid", ssidCsv(), &ssidIndex,
                "The network shown here is the one that gets saved. "
                "'not in range' means the saved network isn't "
                "currently broadcasting.");
  f.addText("SSID Override", "ssidOver", &xSSIDOverride, "",
            "Takes priority over the SSID list above whenever it is "
            "filled in. Leave blank unless the network is hidden.");
  f.addPassword("Network Password", "pw", &xPassword);
  f.addPassword("Live Form Password", "lfpw", &xLiveFormPassword, "Leave blank to disable");
  f.addText("NTP Server", "ntp", &xNtpServ, "Primary time server");
  f.addText("Alternate NTP Server", "ntp2", &xNtpServ2, "Used if primary fails; leave blank to disable");
  f.addDropDown("NTP Polling Interval (minutes)", "ntpPoll", "5, 15, 30, 60, 120, 240", &cfgNtpPollIndex,
                "How often to re-sync from the upstream NTP server. "
                "Does not affect boot or retry timing.");
  f.addToggle("Live Form", "liveform", &cfgLiveForm);
  f.addSubheading("UTC Offset");
  f.addHourMinute("UTC Offset", "utc", &cfgUtcOffset, "0, 30, 45");
  f.addDropDown("UTC Direction", "utcdir", "-, +", &cfgUtcDirection);
  f.addSubheading("Daylight Saving Time");
  f.addDropDown("DST Start Month", "dstm", OPT_MONTHS, &cfgDstStartMonth);
  f.addDropDownRange("DST Start Week", "dstw", 1, 5, &cfgDstStartWeek);
  f.addDropDown("DST Start Day", "dstd", OPT_DAYS, &cfgDstStartDay);
  f.addDropDownRange("DST Start Hour", "dsth", 0, 23, &cfgDstStartHour);
  f.addDropDownRange("DST Offset", "dsto", 0, 1, &cfgDstOffset, "Hours added during DST");
  f.addDropDown("DST Direction", "dstdir", "-, +", &cfgDstDirection);
  f.addSubheading("Standard Time");
  f.addDropDown("ST Start Month", "stdm", OPT_MONTHS, &cfgStdStartMonth);
  f.addDropDownRange("ST Start Week", "stdw", 1, 5, &cfgStdStartWeek);
  f.addDropDown("ST Start Day", "stdd", OPT_DAYS, &cfgStdStartDay);
  f.addDropDownRange("ST Start Hour", "stdh", 0, 23, &cfgStdStartHour);
  f.addSubheading("Time");
  f.addDropDown("Time Display", "tdisp", "12 Hour, 24 Hour, US English Verbose", &cfgTimeDisplay);
  f.addSubheading("OTA Update");
  f.addText("Firmware URL", "ota", &xOtaUrl, "Server URL for OTA updates");
  f.addPage("WordClock-2-6 Settings", "Libraries");
  f.addDropDown("Time and Date Language", "tdlang", "Always Library 1, Current Library", &cfgTimeDateLang);
  f.addPage("WordClock-2-6 Settings", "Display");
  f.addRange("Brightness", "br", 0, 9, &preset[4]);
  f.addPage("WordClock-2-6 Settings", "Information");
  f.addRange("Date Display Start Second", "infosec", 0, 59, &cfgInfoSec);
  f.addDropDown("Date Display", "ddisp", "MM/DD/YY, DD/MM/YY, YY/MM/DD, US English Verbose, Off", &cfgDateDisplay);
  f.addSubheading("Light Sensor");
  f.addToggle("Light Sensor", "lightsensor", &cfgLightSensor, "Auto-adjust from ambient light");
  f.addRange("Min Display Brightness", "lightminbr", 0, 9, &cfgLightMinBr, "Lowest auto brightness in dark");
  f.addRange("Min Backlight Brightness", "lightminled", 0, 9, &cfgLightMinLed, "Lowest auto backlight in dark");
  f.addSubheading("Motion Sensor");
  f.addDropDown("Motion Sensor", "pir", "Off, 15 Seconds, 1 Minute, 5 Minutes, 10 Minutes, 15 Minutes, 30 Minutes, 1 Hour", &cfgPir,
                "Blank display after no motion");
  f.addSubheading("Weather");
  f.addRange("Start Second", "wxsec", 0, 59, &cfgWxSec);
  f.addDropDown("Display Interval", "wxint", "1 Minute, 5 Minutes, 15 Minutes, 1 Hour", &cfgWxInt,
                "How often the weather display appears");
  f.addDropDown("Temperature Units", "wxscale", "Fahrenheit, Celsius, Kelvin", &cfgWxScale);
  f.addDropDown("Pressure Units", "punit", "Inches of Mercury, Millibars", &cfgPressureUnit);
  f.addSubheading("Inside");
  f.addToggle("Temperature", "intemp", &cfgInsideTemp);
  f.addRange("Offset", "tempoff", -15, 15, &cfgTempOffset, "Calibration for BME280 sensor (whole degrees)");
  f.addToggle("Pressure", "inpress", &cfgInsidePress);
  f.addToggle("Humidity", "inhumid", &cfgInsideHumid);
  f.addSubheading("Outside");
  f.addText("Weather Key", "wxkey", &xKey, "Visual Crossing API key");
  f.addText("Postal Code or Lat/Lon", "wxzip", &xpCode);
  f.addText("Country Code (2 Letter)", "wxcc", &xpCountry);
  f.addToggle("Temperature", "outtemp", &cfgOutTemp);
  f.addToggle("Pressure", "outpress", &cfgOutPress);
  f.addToggle("Humidity", "outhumid", &cfgOutHumid);
  f.addToggle("Wind", "outwind", &cfgOutWind);
  f.addToggle("Current Conditions", "currwx", &cfgCurrentWx);
  f.addDropDown("Labels", "wxlabel", "Off, Short, Long", &cfgWxLabels);
  f.addDropDown("Display Duration", "wxtslice", "5 Seconds, 10 Seconds, 20 Seconds", &cfgWxTimeSlice);
  f.setHomePage();
  f.addActionButton("Start", "start", "✓ Starting...");
}

int main() {
  preset[49] = 5; preset[44] = 1; preset[29] = 500; preset[31] = 2; preset[32] = 2; preset[34] = 2; preset[35] = 1;
  preset[36] = 1; preset[37] = 10; preset[38] = 1; preset[40] = 2;
  render("chrome only (CSS/JS/header + Start)", 65536, chromeOnly, 0);
  render("OLD WC-1-4 3.7.10 Setup sub-page (page0)", 65536, wc14Old, 0, "/page0");
  render("OLD WC-1-4 3.7.10 Information sub-page", 65536, wc14Old, 0, "/page3");
  render("OLD WC-1-4 3.7.10 home page (nav only)", 65536, wc14Old, 0, "/");
  render("NEW WordClock-5 4.4.11 one page", 65536, wc5, 0);
  render("NEW WordClock-1-4 3.7.11 one page, 4 libs", 65536, wc14Setup, 4);
  render("NEW WordClock-1-4 3.7.11 one page, 12 libs", 65536, wc14Setup, 12);
  return 0;
}
