/*----------------------------------------------------------------------*
   WebPanel.h - Unified AJAX form library for ESP32

   Generates responsive web forms with live AJAX updates.
   Each field change is sent immediately to the server.
   "Save" persists to NVS via user callback (optionally reboots).
   Works in setup() loop (AP config) or loop() (live settings).

   Supports multi-page forms: main page with nav buttons,
   sub-pages with Save + Back buttons.

   Usage:
     WebPanel form;
     form.setTitle("My Device");
     form.setSaveCallback(mySaveFunc);
     form.setOnChange(myChangeFunc);
     form.begin(&server);
     // in loop or setup: form.handleClient();
  ----------------------------------------------------------------------*/

#ifndef WEBPANEL_H
#define WEBPANEL_H

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiServer.h>

#define WP_DEFAULT_FIELDS 80

// Max options parsed at render time (temporary stack array)
#ifndef WP_MAX_OPTIONS
#define WP_MAX_OPTIONS 50
#endif

#ifndef WP_MAX_PAGES
#define WP_MAX_PAGES 10
#endif

// Static HTML render buffer size — heap-allocated once at boot.
// Must be large enough for the longest single page render.
// Setup form (~70 fields) needs ~35KB. 40KB gives safe headroom.
// If you split fields across pages with addPage(), this can be reduced.
#ifndef WP_HTML_BUFFER_SIZE
#define WP_HTML_BUFFER_SIZE 40960
#endif

enum WPFieldType : uint8_t {
  WP_DROPDOWN,
  WP_RANGE,
  WP_SUBHEADING,
  WP_SEPARATOR,
  WP_COLORPICKER,
  WP_DROPDOWN_OFFSET,
  WP_TEXT,
  WP_PASSWORD,
  WP_CHECKBOX,
  WP_RADIO,
  WP_TIME,
  WP_NUMBER,
  WP_DROPDOWN_RANGE,
  WP_HIDDEN,
  WP_PAGE_BUTTON,
  WP_ACTION_BUTTON,
  WP_TEXT_INPUT
};

struct WPField {
  WPFieldType type;      // 1 byte (enum : uint8_t)
  uint8_t  optionCount;    // 1 byte — max WP_MAX_OPTIONS
  int8_t   page;           // 1 byte — -1 = main, 0+ = sub-page index
  bool     includeSeconds; // 1 byte
  const char* thumbColor;  // 4 bytes — "r","g","b" for channel tint, or CSS color string, or nullptr
  int16_t  minVal;         // 2 bytes
  int16_t  maxVal;         // 2 bytes
  int16_t  step;           // 2 bytes
  int16_t  offset;         // 2 bytes
  String   label;          // 12
  String   fieldName;      // 12 (also serves as HTML id for range sliders)
  String   optionsCSV;     // 12 (parsed at render time)
  String   extraText;      // 12 (placeholder OR buttonLabel — only one used per field type)
  const char* tip;          // 4 (tooltip text — nullptr = no tooltip icon rendered)
  int*     presetPtr;      // 4
  String*  strPtr;         // 4
  bool     (*condition)(); // 4
};

typedef void (*WPSaveCallback)();
typedef void (*WPChangeCallback)(const String& field, int value);
typedef void (*WPTextCallback)(const String& field, const String& value);

class WebPanel {
public:
  WebPanel();
  ~WebPanel();

  // Allocate the static HTML render buffer. Call ONCE early in setup()
  // before any other heap activity to prevent fragmentation.
  static void allocBuffer();

  // Free the static HTML render buffer (e.g. before OTA to reclaim heap).
  // allocBuffer() can be called again afterwards to re-allocate.
  static void freeBuffer();

  // Read-only access to the render buffer pointer (for diagnostic logging,
  // e.g. confirming the buffer landed in PSRAM vs DRAM).
  static const char* bufferPtr() { return _htmlBuf; }

  // Set the maximum number of fields for this instance. Call before any
  // add*() calls. If not called, defaults to WP_DEFAULT_FIELDS (80).
  void setMaxFields(int maxFields);

  // Set the home page header. Two-line variant: line1 (large) + line2 (small).
  // Single-arg variant sets line1 only.
  void setTitle(const String& line1, const String& line2 = "");
  void setSaveCallback(WPSaveCallback cb);
  void setOnChange(WPChangeCallback cb);
  void setOnTextChange(WPTextCallback cb);
  void setRebootOnSave(bool reboot);
  void setAuth(String* password);  // optional HTTP Basic Auth — enforced when *password is non-empty
  void setSliderStyle(int trackHeight, int thumbSize);  // slider dimensions in px (default: 6px track, 22px thumb)
  void begin(WiFiServer* server);
  void handleClient();

  // Page management. Two-line variant: line1 (large) + line2 (small).
  // Single-arg variant sets line1 only and leaves line2 empty.
  // The nav button label uses line2 if set, otherwise line1.
  void addPage(const String& line1, const String& line2 = "");
  void setHomePage();  // switch back to home page for subsequent add* calls

  // Action button — always added to the home page (regardless of current
  // page). When clicked it fires the change callback with the given
  // fieldName and value=1, so the app can take action. Does NOT cause the
  // home page Save button to appear.
  // If confirmMessage is non-empty, clicking the button replaces the entire
  // page with that message (use for actions that reboot the device).
  void addActionButton(const String& label, const String& fieldName,
                       const String& confirmMessage = "");

  // Display a message overlay in the browser using the same style/animation
  // as the "Settings Saved" overlay. Call from inside any callback (change,
  // text, save). The message is returned as the AJAX response body and the
  // browser displays it briefly then fades out. Calling this overrides the
  // default "Settings Saved" overlay on save callbacks.
  void showMessage(const String& text);

  // Field types — every interactive field accepts an optional `tip` string.
  // Non-empty tip → an info icon (ⓘ) appears next to the label and tapping
  // it shows a floating tooltip bubble with the text.
  void addDropDown(const String& label, const String& field,
                   const String& options, int* preset,
                   const char* tip = nullptr);
  void addDropDownOffset(const String& label, const String& field,
                         const String& options, int* preset, int offset,
                         const char* tip = nullptr);
  void addRange(const String& label, const String& field,
                int minVal, int maxVal, int* preset,
                const char* tip = nullptr, const char* thumbColor = nullptr);
  void addSubheading(const String& text);
  void addConditionalSubheading(bool (*condition)(), const String& text);
  void addColorPicker(const String& label, const String& field, int* preset,
                      const char* tip = nullptr);
  void addSeparator();
  void addConditionalDropDown(bool (*condition)(),
                               const String& label, const String& field,
                               const String& options, int* preset,
                               const char* tip = nullptr);
  void addText(const String& label, const String& field, String* ptr,
               const String& placeholder = "",
               const char* tip = nullptr);
  void addPassword(const String& label, const String& field, String* ptr,
                   const char* tip = nullptr);
  void addTextInput(const String& label, const String& field, String* ptr,
                    const String& placeholder = "", int maxLen = 63,
                    const String& buttonLabel = "Send", const char* tip = nullptr);
  void addCheckbox(const String& label, const String& field, int* preset,
                   const char* tip = nullptr);
  void addRadio(const String& label, const String& field,
                const String& options, int* preset,
                const char* tip = nullptr);
  void addTime(const String& label, const String& field, int* preset,
               bool includeSeconds = false,
               const char* tip = nullptr);
  void addNumber(const String& label, const String& field,
                 int minVal, int maxVal, int step, int* preset,
                 const char* tip = nullptr);
  void addDropDownRange(const String& label, const String& field,
                        int minVal, int maxVal, int* preset,
                        const char* tip = nullptr);
  void addHidden(const String& field, int* preset);

private:
  WiFiServer*        _server;
  String             _titleLine1;   // home page large header
  String             _titleLine2;   // home page small header (under line1)
  String             _pendingMessage; // text returned in next AJAX response
  WPSaveCallback   _saveCb;
  WPChangeCallback _changeCb;
  WPTextCallback   _textCb;
  String*          _authPass;     // optional — if non-null and non-empty, require HTTP Basic Auth
  WPField*         _fields;
  int              _maxFields;
  int              _fieldCount;

  // Page state
  String _pageLine1[WP_MAX_PAGES];   // sub-page large header
  String _pageLine2[WP_MAX_PAGES];   // sub-page small header (under line1)
  int    _numPages;
  int    _currentPage;     // page being built: -1 = main
  bool   _mainHasFields;
  bool   _rebootOnSave;
  int    _sliderTrack;     // track height in px (default 6)
  int    _sliderThumb;     // thumb diameter in px (default 22)

  void ensureFields();  // auto-allocate fields array on first use
  static int countOptions(const String& csv);
  void parseOptions(const String& csv, String out[], int& count);
  void serveForm(WiFiClient& client, int page);
  void handleAjax(WiFiClient& client, const String& req);
  void handleSave(WiFiClient& client);
  void sendOK(WiFiClient& client);

  // HTML buffer — allocated once on heap at begin() time, shared across
  // all instances. A single early allocation doesn't fragment the heap.
  static char* _htmlBuf;
  static int   _htmlBufSize;
  int          _htmlPos;

  // Append helpers — write to _htmlBuf with bounds checking
  void out(const char* s);
  void out(const String& s);
  void out(int v);

  // HTML generators (write to _htmlBuf via out())
  void genDropDown(int idx);
  void genRange(int idx);
  void genSubheading(int idx);
  void genColorPicker(int idx);
  void genSeparator();
  void genText(int idx);
  void genPassword(int idx);
  void genCheckbox(int idx);
  void genRadio(int idx);
  void genTime(int idx);
  void genNumber(int idx);
  void genDropDownRange(int idx);
  void genHidden(int idx);
  void genPageButton(int idx);
  void genActionButton(int idx);
  void genTextInput(int idx);

  // Tooltip helpers — emit info icon (inside label) and floating tooltip
  // box (after label). No-op if the field has no tip text.
  void emitTipIcon(int idx);
  void emitTipBox(int idx);

  bool checkAuth(const String& headers);
  void send401(WiFiClient& client);
  static String urlDecode(const String& input);
};

#endif
