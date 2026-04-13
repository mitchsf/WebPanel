/*----------------------------------------------------------------------*
   WebPanelTest.ino — Feature demo for the WebPanel library

   Creates an AP ("WebPanel-Test"), serves a multi-page form at
   192.168.4.1 showing every field type, tooltips, action buttons,
   conditional fields, showMessage(), and auth.

   To run: copy this file into its own sketch folder
   (WebPanelTest/WebPanelTest.ino) and compile for any ESP32 board.
  ----------------------------------------------------------------------*/

#include <WiFi.h>
#include <WebPanel.h>

WiFiServer server(80);
WebPanel panel;

// ---------- Bound variables ----------

// Display page
int brightness    = 5;
int contrast      = 3;
int displayMode   = 1;
int colorTemp     = 2;
int refreshRate   = 60;
int accentColor   = 0xFF8000;
int showFPS       = 0;

// Lighting page
int ledEffect     = 3;
int ledBrightness = 7;
int ledFollows    = 0;
int underlightBr  = 5;
int underlightCol = 0x0000FF;

// Schedule page
int scheduleOn    = 0700;
int scheduleOff   = 2300;
int dayOfWeek     = 0;
int startSecond   = 30;
int pirTimeout    = 3;
int lightSensor   = 1;
int minBrightness = 2;

// Network page
String deviceName = "Test Device";
String ssid       = "";
String password   = "";
String ntpServer  = "pool.ntp.org";

// Advanced page
int advancedMode  = 0;
int debugLevel    = 0;
int logInterval   = 5;
int hiddenState   = 42;

// Auth
String formPassword = "";  // empty = no auth

// ---------- Callbacks ----------

void onChange(const String& field, int v) {
  Serial.print("[change] "); Serial.print(field);
  Serial.print(" = "); Serial.println(v);

  if (field == "brightness") {
    panel.showMessage("Brightness set to " + String(v));
  }
  else if (field == "reboot") {
    Serial.println("Reboot requested!");
  }
  else if (field == "test") {
    panel.showMessage("Test complete");
  }
  else if (field == "reset") {
    Serial.println("Factory reset requested!");
  }
}

void onText(const String& field, const String& v) {
  Serial.print("[text] "); Serial.print(field);
  Serial.print(" = "); Serial.println(v);
}

void onSave() {
  Serial.println("[save] Settings saved");
  server.stop(); delay(50); server.begin();  // reclaim leaked sockets
}

// ---------- Conditional field ----------

bool isAdvanced() { return advancedMode == 1; }

// ---------- Setup ----------

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n--- WebPanel Test ---");

  WebPanel::allocBuffer();  // FIRST — before WiFi

  WiFi.softAP("WebPanel-Test");
  Serial.print("AP IP: ");
  Serial.println(WiFi.softAPIP());

  panel.setTitle("WebPanel Test", "v1.0 Demo");
  panel.setAuth(&formPassword);
  panel.setSaveCallback(onSave);
  panel.setOnChange(onChange);
  panel.setOnTextChange(onText);
  panel.begin(&server);
  server.begin();

  // ============================================================
  // PAGE: Display
  // ============================================================
  panel.addPage("WebPanel Test", "Display");

  panel.addRange("Brightness", "brightness", 0, 9, &brightness,
    "0 = off, 9 = maximum. Adjusts the display PWM duty cycle.");
  panel.addRange("Contrast", "contrast", 0, 9, &contrast);
  panel.addDropDown("Display Mode", "dispmode",
    "Off, Normal, Eco, High Contrast, Night", &displayMode,
    "Eco reduces refresh rate to save power");
  panel.addDropDown("Color Temperature", "colortemp",
    "Warm, Neutral, Cool, Daylight", &colorTemp);
  panel.addNumber("Refresh Rate", "refresh", 30, 120, 10, &refreshRate,
    "Hz — higher values are smoother but use more power");
  panel.addColorPicker("Accent Color", "accent", &accentColor);
  panel.addCheckbox("Show FPS Counter", "showfps", &showFPS);

  // ============================================================
  // PAGE: Lighting
  // ============================================================
  panel.addPage("WebPanel Test", "Lighting");

  panel.addSubheading("Digit LEDs");
  panel.addDropDown("LED Effect", "ledeffect",
    "Off, Solid, Breathe, Rainbow, Chase, Sparkle, America Wave, "
    "Random Color, Random Multi, Larson Scanner", &ledEffect);
  panel.addRange("LED Brightness", "ledbr", 0, 9, &ledBrightness);
  panel.addDropDown("LED Follows Digit", "ledfollows",
    "Off, On", &ledFollows,
    "When on, LED color matches the active digit value");

  panel.addSeparator();

  panel.addSubheading("Under Light");
  panel.addRange("Brightness", "ulbr", 0, 9, &underlightBr);
  panel.addColorPicker("Color", "ulcol", &underlightCol);

  // ============================================================
  // PAGE: Schedule
  // ============================================================
  panel.addPage("WebPanel Test", "Schedule");

  panel.addTime("On Time", "ontime", &scheduleOn, false,
    "Device activates at this time daily");
  panel.addTime("Off Time", "offtime", &scheduleOff);
  panel.addDropDown("Day of Week", "dow",
    "Every Day, Weekdays, Weekends, Mon, Tue, Wed, Thu, Fri, Sat, Sun",
    &dayOfWeek);
  panel.addDropDownRange("Start Second", "startsec", 0, 59, &startSecond);

  panel.addSeparator();

  panel.addSubheading("Motion Sensor");
  panel.addDropDown("PIR Timeout", "pir",
    "Off, 10 Seconds, 20 Seconds, 30 Seconds, 1 Minute, "
    "5 Minutes, 10 Minutes, 30 Minutes", &pirTimeout);

  panel.addSubheading("Light Sensor");
  panel.addDropDown("Light Sensor", "lightsensor", "Off, On", &lightSensor);
  panel.addRange("Min Brightness", "minbr", 0, 9, &minBrightness,
    "Display won't dim below this level when light sensor is active");

  // ============================================================
  // PAGE: Network
  // ============================================================
  panel.addPage("WebPanel Test", "Network");

  panel.addText("Device Name", "devname", &deviceName, "My Device");
  panel.addText("WiFi SSID", "ssid", &ssid, "network name");
  panel.addPassword("WiFi Password", "pw", &password);
  panel.addText("NTP Server", "ntp", &ntpServer, "pool.ntp.org");
  panel.addPassword("Form Password", "formpw", &formPassword,
    "Leave blank to disable authentication");

  // ============================================================
  // PAGE: Advanced
  // ============================================================
  panel.addPage("WebPanel Test", "Advanced");

  panel.addRadio("Mode", "advmode", "Standard, Advanced", &advancedMode);

  panel.addConditionalSubheading(isAdvanced, "Debug Settings");
  panel.addConditionalDropDown(isAdvanced,
    "Debug Level", "debug", "Off, Errors, Warnings, Verbose", &debugLevel);

  panel.addNumber("Log Interval (sec)", "logint", 1, 60, 1, &logInterval);
  panel.addHidden("hiddenState", &hiddenState);

  // ============================================================
  // Action buttons (always on home page)
  // ============================================================
  panel.addActionButton("Test Buzzer", "test");
  panel.addActionButton("Reboot", "reboot", "\u2713 Rebooting...");
  panel.addActionButton("Factory Reset", "reset", "Resetting to defaults...");

  Serial.println("Ready — connect to WiFi 'WebPanel-Test' and open 192.168.4.1");
}

void loop() {
  panel.handleClient();
}
