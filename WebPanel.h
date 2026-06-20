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
  WP_TEXT_INPUT,
  WP_HTML,
  WP_BUTTON
};

struct WPField {
  WPFieldType type;      // 1 byte (enum : uint8_t)
  uint8_t  optionCount;    // 1 byte — max WP_MAX_OPTIONS
  int8_t   page;           // 1 byte — -1 = main, 0+ = sub-page index
  bool     includeSeconds; // 1 byte — addTime only (HH:MM vs HH:MM:SS)
  bool     clearable;      // 1 byte — addTextInput only: render an inline "x" clear button
  bool     reloadAfter;    // 1 byte — addActionButton only: poll then reload after the overlay (vs fade to blank)
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
  // Save button: when reboot is true the JS shows the "Settings Saved" overlay
  // immediately (the device is expected to reboot in the save callback). The
  // optional saveLabel overrides the default "Save Settings" button text on
  // both the home page and sub-pages — pass "" (default) to keep it unchanged.
  void setRebootOnSave(bool reboot, const String& saveLabel = "");
  void setAuth(String* password);  // optional HTTP Basic Auth — enforced when *password is non-empty
  static void setBufferSize(int bytes);  // override render-buffer size (call BEFORE allocBuffer()).
                                         // Shrinking it frees DRAM for the WiFi/TCP TX path on
                                         // no-PSRAM boards, where a too-large buffer can starve the
                                         // send side and stall large responses. Default WP_HTML_BUFFER_SIZE.
  void setCaptivePortal(bool on);  // AP/setup mode: 302-redirect any non-form request to the form root
                                   // so OS connectivity probes trigger the "Sign in to network" popup.
                                   // Default off — STA/runtime forms are unaffected.
  void setSliderStyle(int trackHeight, int thumbSize);  // slider dimensions in px (default: 6px track, 22px thumb)
  void begin(WiFiServer* server);
  void handleClient();

  // Page management. Two-line variant: line1 (large) + line2 (small).
  // Single-arg variant sets line1 only and leaves line2 empty.
  // The nav button label uses buttonLabel if set, otherwise line2 if set,
  // otherwise line1. Use buttonLabel when line2 is HTML (e.g. a flex header
  // with version + IP) so the home-page button still shows readable text.
  // buttonColor: optional CSS color (e.g. "var(--sc)" or "#10b981") for the
  // page-nav button on the home page. nullptr → default blue (.page-btn).
  void addPage(const String& line1, const String& line2 = "",
               const String& buttonLabel = "",
               const char* buttonColor = nullptr);
  void setHomePage();  // switch back to home page for subsequent add* calls

  // Action button — always added to the home page (regardless of current
  // page). When clicked it fires the change callback with the given
  // fieldName and value=1, so the app can take action. Does NOT cause the
  // home page Save button to appear.
  // If confirmMessage is non-empty, clicking the button replaces the entire
  // page with that message (use for actions that reboot the device).
  // If reloadAfter is true, the page polls the server after showing the
  // overlay and navigates to the HOME page as soon as it responds again — use
  // for actions that may NOT reboot (e.g. an OTA check that finds the firmware
  // already current) so the UI comes back instead of fading to a blank page.
  // It returns to home (not the current sub-page) because device-level status
  // typically lives there. Reboot-and-switch actions should leave it false so
  // the overlay (with reconnect instructions) stays.
  // If statusField is non-empty (only meaningful with reloadAfter), the poll
  // hits "/?field=<statusField>" instead of "/?ping=1": its text response is
  // the action's RESULT. The overlay shows that result for a few seconds
  // (instead of relying on a status box on the home page) and then navigates
  // home. A response of "" or "OK" means "no result" → navigate immediately.
  // The blocking action won't answer the poll until it finishes, so the first
  // non-error response is the genuine outcome (or a post-reboot empty result).
  void addActionButton(const String& label, const String& fieldName,
                       const String& confirmMessage = "",
                       bool reloadAfter = false,
                       const String& statusField = "");

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
  void addConditionalRange(bool (*condition)(),
                           const String& label, const String& field,
                           int minVal, int maxVal, int* preset,
                           const char* tip = nullptr, const char* thumbColor = nullptr);
  void addConditionalColorPicker(bool (*condition)(),
                                 const String& label, const String& field,
                                 int* preset, const char* tip = nullptr);
  void addText(const String& label, const String& field, String* ptr,
               const String& placeholder = "",
               const char* tip = nullptr);
  void addPassword(const String& label, const String& field, String* ptr,
                   const char* tip = nullptr);
  // Single-line text input with a submit button (rows defaults to 1).
  // Setting rows > 1 renders a multi-line <textarea> instead, full-width.
  // clearable=true (single-line only) inserts a small inline "x" button
  // between the input and the submit button — clicking it clears the
  // input value and sends an empty string to the server.
  void addTextInput(const String& label, const String& field, String* ptr,
                    const String& placeholder = "", int maxLen = 63,
                    const String& buttonLabel = "Send", const char* tip = nullptr,
                    int rows = 1, bool clearable = false);
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

  // In-page button. Unlike addActionButton (always renders on the home page),
  // addButton renders on the current page (whatever addPage() was last called
  // for). On click, fires the change callback with the given fieldName and
  // value=1 — no state to update on the server, so no preset pointer needed.
  void addButton(const String& label, const String& fieldName,
                 const char* tip = nullptr);

  // Change a field's visible label by field name. The label String is re-read
  // on every page render, so the new text appears on the next request/reload
  // (use a "::RL::" showMessage to force that reload). Useful for buttons that
  // toggle state (e.g. Start/Stop). No-op if the field name isn't found.
  void setFieldLabel(const String& fieldName, const String& label);

  // Inject raw HTML into the page at the position of this call.
  // The HTML is written verbatim to the page (no escaping). Use for
  // custom widgets like tables, charts, or status displays the
  // built-in field types can't express. The pointed-to String is
  // re-read on every page render, so callers can mutate it at runtime
  // and the next render reflects the new content.
  void addHTML(String* htmlPtr);

  // Cumulative request counters (since boot). Surfaced via /health and
  // also useful for on-device diagnostics (e.g. tube scroll).
  static uint32_t requestOK()       { return _reqOK; }
  static uint32_t requestRejected() { return _reqRejected; }

  // Field gating: declare that `gatedField` should be enabled only when
  // `controllerField` (typically a dropdown) currently has `enableValue`.
  // The library emits the disable/enable JS automatically. Idempotent;
  // safe to call multiple times for the same gated field (last wins).
  // String literals or other stable storage required for the field names
  // (same convention as fieldName arguments elsewhere in this API).
  void gateFieldBy(const char* gatedField,
                   const char* controllerField,
                   int32_t enableValue);

private:
  WiFiServer*        _server;
  String             _titleLine1;   // home page large header
  String             _titleLine2;   // home page small header (under line1)
  String             _pendingMessage; // text returned in next AJAX response
  WPSaveCallback   _saveCb;
  WPChangeCallback _changeCb;
  WPTextCallback   _textCb;
  String*          _authPass;     // optional — if non-null and non-empty, require HTTP Basic Auth
  bool             _captivePortal = false;  // AP captive-portal redirect (see setCaptivePortal)
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
  String _saveLabel;       // overrides "Save Settings" button text when non-empty
  int    _sliderTrack;     // track height in px (default 6)
  int    _sliderThumb;     // thumb diameter in px (default 22)

  // Field-gating relationships registered via gateFieldBy(). Each entry
  // says: the input with id == gatedField is disabled unless the input
  // with id == controllerField has value == enableValue. Emitted as JS
  // in the form's universal <script> block.
  struct GatePair {
    const char* gatedField;
    const char* controllerField;
    int32_t     enableValue;
  };
  static const int WP_MAX_GATE_PAIRS = 8;
  GatePair _gatePairs[WP_MAX_GATE_PAIRS];
  int      _gatePairCount = 0;

  void ensureFields();  // auto-allocate fields array on first use
  static int countOptions(const String& csv);
  void parseOptions(const String& csv, String out[], int& count);
  void serveForm(WiFiClient& client, int page);
  void writeAll(WiFiClient& client, const uint8_t* buf, int len);  // chunked, slow-link-safe body send
  void handleAjax(WiFiClient& client, const String& req);
  void handleSave(WiFiClient& client);
  void handleHealth(WiFiClient& client);
  void sendOK(WiFiClient& client);

  // Request counters surfaced by /health.
  static uint32_t _reqOK;
  static uint32_t _reqRejected;

  // HTML buffer — allocated once on heap at begin() time, shared across
  // all instances. A single early allocation doesn't fragment the heap.
  static char* _htmlBuf;
  static int   _htmlBufSize;
  static int   _wantBufSize;   // desired render-buffer size; set via setBufferSize() before allocBuffer()
  int          _htmlPos;

  // Per-request input buffers — static so they're reserve()'d once and the
  // capacity is preserved across requests. Avoids per-request alloc churn
  // from String growth inside readStringUntil()/readString().
  static String _reqBuf;
  static String _hdrBuf;

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
  void genHTML(int idx);
  void genButton(int idx);

  // Tooltip helpers — emit info icon (inside label) and floating tooltip
  // box (after label). No-op if the field has no tip text.
  void emitTipIcon(int idx);
  void emitTipBox(int idx);

  bool checkAuth(const String& headers);
  void send401(WiFiClient& client);
  static String urlDecode(const String& input);
};

#endif
