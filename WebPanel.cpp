/*----------------------------------------------------------------------*
   WebPanel.cpp - Unified AJAX form library for ESP32
  ----------------------------------------------------------------------*/

#include "WebPanel.h"
#include "mbedtls/base64.h"
#include <sys/socket.h>

// Static HTML render buffer — allocated once on heap via begin().
// Shared across all instances.
char* WebPanel::_htmlBuf = nullptr;
int   WebPanel::_htmlBufSize = 0;
int   WebPanel::_wantBufSize = WP_HTML_BUFFER_SIZE;
uint32_t WebPanel::_reqOK       = 0;
uint32_t WebPanel::_reqRejected = 0;

// Static per-request input buffers — reserve()'d once in allocBuffer().
String WebPanel::_reqBuf;
String WebPanel::_hdrBuf;

WebPanel::WebPanel()
  : _server(nullptr), _titleLine1("Settings"), _titleLine2(""),
    _pendingMessage(""),
    _saveCb(nullptr), _changeCb(nullptr), _textCb(nullptr), _authPass(nullptr),
    _fields(nullptr), _maxFields(0), _fieldCount(0),
    _numPages(0), _currentPage(-1),
    _mainHasFields(false), _rebootOnSave(false),
    _sliderTrack(6), _sliderThumb(22), _htmlPos(0)
{}

WebPanel::~WebPanel() {
  free(_fields);
}

void WebPanel::setMaxFields(int maxFields) {
  if (_fields) free(_fields);
  _maxFields = maxFields;
  _fields = (WPField*)calloc(maxFields, sizeof(WPField));
  _fieldCount = 0;
}

void WebPanel::ensureFields() {
  if (!_fields) setMaxFields(WP_DEFAULT_FIELDS);
}

void WebPanel::setTitle(const String& line1, const String& line2) {
  _titleLine1 = line1;
  _titleLine2 = line2;
}

void WebPanel::showMessage(const String& text) {
  _pendingMessage = text;
}
void WebPanel::setSaveCallback(WPSaveCallback cb) { _saveCb = cb; }
void WebPanel::setOnChange(WPChangeCallback cb) { _changeCb = cb; }
void WebPanel::setOnTextChange(WPTextCallback cb) { _textCb = cb; }
void WebPanel::setAuth(String* password) { _authPass = password; }
void WebPanel::setBufferSize(int bytes) { _wantBufSize = bytes; }
void WebPanel::setCaptivePortal(bool on) { _captivePortal = on; }
void WebPanel::setRebootOnSave(bool reboot) { _rebootOnSave = reboot; }

void WebPanel::gateFieldBy(const char* gatedField,
                            const char* controllerField,
                            int32_t enableValue) {
  // Idempotent: if this gatedField is already registered, update its
  // controller / value. Otherwise append a new entry.
  for (int i = 0; i < _gatePairCount; ++i) {
    if (strcmp(_gatePairs[i].gatedField, gatedField) == 0) {
      _gatePairs[i].controllerField = controllerField;
      _gatePairs[i].enableValue     = enableValue;
      return;
    }
  }
  if (_gatePairCount >= WP_MAX_GATE_PAIRS) return;
  _gatePairs[_gatePairCount].gatedField      = gatedField;
  _gatePairs[_gatePairCount].controllerField = controllerField;
  _gatePairs[_gatePairCount].enableValue     = enableValue;
  _gatePairCount++;
}

void WebPanel::setSliderStyle(int trackHeight, int thumbSize) {
  _sliderTrack = trackHeight;
  _sliderThumb = thumbSize;
}

void WebPanel::begin(WiFiServer* server) {
  _server = server;
  ensureFields();
  // Buffer should already be allocated by allocBuffer() at the start of
  // setup() to keep it at the bottom of the heap (no fragmentation).
  // Fall back to allocating here if not.
  if (_htmlBuf == nullptr) allocBuffer();
}

// Static — call ONCE from setup() before any other heap activity.
// Prefers PSRAM when available (ESP32 PICO-V3-02 etc.) so the 40 KB render
// buffer does not consume scarce internal DRAM. Falls back to DRAM on chips
// without PSRAM (PICO-D4, PICO-V3).
void WebPanel::allocBuffer() {
  if (_htmlBuf != nullptr) return;
#if defined(BOARD_HAS_PSRAM) || defined(CONFIG_SPIRAM) || defined(CONFIG_SPIRAM_SUPPORT)
  if (psramFound()) {
    _htmlBuf = (char*)ps_malloc(_wantBufSize);
  }
#endif
  if (_htmlBuf == nullptr) {
    _htmlBuf = (char*)malloc(_wantBufSize);
  }
  if (_htmlBuf) {
    _htmlBufSize = _wantBufSize;
    _htmlBuf[0] = 0;
  }
  // Pre-size input buffers right after the render buffer, while the heap is
  // still pristine. Each gets one 1 KB allocation, reused across requests.
  _reqBuf.reserve(1024);
  _hdrBuf.reserve(1024);
}

void WebPanel::freeBuffer() {
  if (_htmlBuf) {
    free(_htmlBuf);
    _htmlBuf = nullptr;
    _htmlBufSize = 0;
  }
}

// -- URL decode ----------------------------------------------------------

// -- HTML buffer append helpers ------------------------------------------

void WebPanel::out(const char* s) {
  if (!s || !_htmlBuf) return;
  int max = _htmlBufSize - 1;
  while (*s && _htmlPos < max) {
    _htmlBuf[_htmlPos++] = *s++;
  }
  _htmlBuf[_htmlPos] = 0;
}

void WebPanel::out(const String& s) {
  out(s.c_str());
}

void WebPanel::out(int v) {
  char tmp[12];
  itoa(v, tmp, 10);
  out(tmp);
}

// -- URL decode -----------------------------------------------------------

String WebPanel::urlDecode(const String& input) {
  String decoded;
  decoded.reserve(input.length());
  for (int i = 0; i < (int)input.length(); i++) {
    if (input[i] == '+') {
      decoded += ' ';
    } else if (input[i] == '%' && i + 2 < (int)input.length()) {
      char hex[3] = { input[i + 1], input[i + 2], 0 };
      decoded += (char)strtol(hex, nullptr, 16);
      i += 2;
    } else {
      decoded += input[i];
    }
  }
  return decoded;
}

// -- Option parser -------------------------------------------------------

// Count options in a CSV string
int WebPanel::countOptions(const String& csv) {
  if (csv.length() == 0) return 0;
  int count = 1;
  for (int i = 0; i < (int)csv.length(); i++) {
    if (csv[i] == ',') count++;
  }
  return count;
}

// Parse CSV into array (used at render time with stack-allocated array)
void WebPanel::parseOptions(const String& csv, String out[], int& count) {
  count = 0;
  int start = 0;
  for (int i = 0; i <= (int)csv.length(); i++) {
    if (i == (int)csv.length() || csv[i] == ',') {
      if (count < WP_MAX_OPTIONS) {
        out[count] = csv.substring(start, i);
        out[count].trim();
        count++;
      }
      start = i + 1;
    }
  }
}

// -- Page management -----------------------------------------------------

void WebPanel::addPage(const String& line1, const String& line2, const String& buttonLabel,
                       const char* buttonColor) {
  if (_numPages >= WP_MAX_PAGES) return;
  ensureFields();

  // Nav button label priority: explicit buttonLabel arg → line2 → line1.
  // The explicit-arg path lets callers put rich HTML in line2 (e.g. a flex
  // header showing version + IP) without that HTML leaking into the button.
  String chosen = buttonLabel.length() > 0
                    ? buttonLabel
                    : (line2.length() > 0 ? line2 : line1);

  // Add a page button field on the main page
  if (_fieldCount < _maxFields) {
    WPField& f = _fields[_fieldCount++];
    f.type = WP_PAGE_BUTTON;
    f.label = chosen;
    f.fieldName = "";
    f.presetPtr = nullptr;
    f.strPtr = nullptr;
    f.condition = nullptr;
    f.page = -1;  // button lives on main page
    f.offset = _numPages;  // store page index
    f.thumbColor = buttonColor;   // optional CSS color override for the nav button
  }

  _pageLine1[_numPages] = line1;
  _pageLine2[_numPages] = line2;
  _currentPage = _numPages;
  _numPages++;
}

void WebPanel::setHomePage() {
  _currentPage = -1;
}


void WebPanel::addActionButton(const String& label, const String& fieldName,
                                const String& confirmMessage, bool reloadAfter,
                                const String& statusField) {
  ensureFields();
  if (_fieldCount >= _maxFields) return;
  WPField& f = _fields[_fieldCount++];
  f.type = WP_ACTION_BUTTON;
  f.label = label;
  f.fieldName = fieldName;
  f.extraText = confirmMessage;   // reused: confirm message text
  f.reloadAfter = reloadAfter;    // confirm-and-clear: poll+reload instead of fade-to-blank
  f.optionsCSV = statusField;     // reused (action button): poll field whose response is the result
  f.presetPtr = nullptr;
  f.strPtr = nullptr;
  f.condition = nullptr;
  f.page = _currentPage;   // home page (-1) by default; sub-page if inside addPage…setHomePage
  // Do NOT set _mainHasFields — action button does not trigger the
  // home-page Save button. (Sub-page Save buttons are emitted unconditionally
  // for any sub-page with field content, so action buttons on a sub-page do
  // not affect that decision either.)
}

// -- Original field builders (backwards compatible) ----------------------

void WebPanel::addDropDown(const String& label, const String& field,
                                   const String& options, int* preset,
                                   const char* tip) {
  ensureFields();
  if (_fieldCount >= _maxFields) return;
  WPField& f = _fields[_fieldCount++];
  f.type = WP_DROPDOWN;
  f.label = label;
  f.fieldName = field;
  f.presetPtr = preset;
  f.strPtr = nullptr;
  f.offset = 0;
  f.tip = tip;
  f.condition = nullptr;
  f.page = _currentPage;
  f.optionsCSV = options;
  f.optionCount = countOptions(options);
  if (_currentPage == -1) _mainHasFields = true;
}

void WebPanel::addDropDownOffset(const String& label, const String& field,
                                         const String& options, int* preset, int offset,
                                         const char* tip) {
  ensureFields();
  if (_fieldCount >= _maxFields) return;
  WPField& f = _fields[_fieldCount++];
  f.type = WP_DROPDOWN_OFFSET;
  f.label = label;
  f.fieldName = field;
  f.presetPtr = preset;
  f.strPtr = nullptr;
  f.offset = offset;
  f.tip = tip;
  f.condition = nullptr;
  f.page = _currentPage;
  f.optionsCSV = options;
  f.optionCount = countOptions(options);
  if (_currentPage == -1) _mainHasFields = true;
}

void WebPanel::addRange(const String& label, const String& field,
                                int minVal, int maxVal, int* preset,
                                const char* tip, const char* thumbColor) {
  ensureFields();
  if (_fieldCount >= _maxFields) return;
  WPField& f = _fields[_fieldCount++];
  f.type = WP_RANGE;
  f.label = label;
  f.fieldName = field;
  f.minVal = minVal;
  f.maxVal = maxVal;
  f.step = 1;
  f.presetPtr = preset;
  f.strPtr = nullptr;
  f.offset = 0;
  f.tip = tip;
  f.thumbColor = thumbColor;
  f.condition = nullptr;
  f.page = _currentPage;
  if (_currentPage == -1) _mainHasFields = true;
}

void WebPanel::addSubheading(const String& text) {
  ensureFields();
  if (_fieldCount >= _maxFields) return;
  WPField& f = _fields[_fieldCount++];
  f.type = WP_SUBHEADING;
  f.label = text;
  f.presetPtr = nullptr;
  f.strPtr = nullptr;
  f.condition = nullptr;
  f.page = _currentPage;
}

void WebPanel::addConditionalSubheading(bool (*condition)(), const String& text) {
  ensureFields();
  if (_fieldCount >= _maxFields) return;
  WPField& f = _fields[_fieldCount++];
  f.type = WP_SUBHEADING;
  f.label = text;
  f.presetPtr = nullptr;
  f.strPtr = nullptr;
  f.condition = condition;
  f.page = _currentPage;
}

void WebPanel::addSeparator() {
  ensureFields();
  if (_fieldCount >= _maxFields) return;
  WPField& f = _fields[_fieldCount++];
  f.type = WP_SEPARATOR;
  f.presetPtr = nullptr;
  f.strPtr = nullptr;
  f.condition = nullptr;
  f.page = _currentPage;
}

void WebPanel::addColorPicker(const String& label, const String& field, int* preset,
                              const char* tip) {
  ensureFields();
  if (_fieldCount >= _maxFields) return;
  WPField& f = _fields[_fieldCount++];
  f.type = WP_COLORPICKER;
  f.label = label;
  f.fieldName = field;
  f.presetPtr = preset;
  f.strPtr = nullptr;
  f.offset = 0;
  f.tip = tip;
  f.condition = nullptr;
  f.page = _currentPage;
  if (_currentPage == -1) _mainHasFields = true;
}

void WebPanel::addConditionalDropDown(bool (*condition)(),
                                              const String& label, const String& field,
                                              const String& options, int* preset,
                                              const char* tip) {
  ensureFields();
  if (_fieldCount >= _maxFields) return;
  WPField& f = _fields[_fieldCount++];
  f.type = WP_DROPDOWN;
  f.label = label;
  f.fieldName = field;
  f.presetPtr = preset;
  f.strPtr = nullptr;
  f.offset = 0;
  f.tip = tip;
  f.condition = condition;
  f.page = _currentPage;
  f.optionsCSV = options;
  f.optionCount = countOptions(options);
  if (_currentPage == -1) _mainHasFields = true;
}

void WebPanel::addConditionalRange(bool (*condition)(),
                                   const String& label, const String& field,
                                   int minVal, int maxVal, int* preset,
                                   const char* tip, const char* thumbColor) {
  ensureFields();
  if (_fieldCount >= _maxFields) return;
  WPField& f = _fields[_fieldCount++];
  f.type = WP_RANGE;
  f.label = label;
  f.fieldName = field;
  f.minVal = minVal;
  f.maxVal = maxVal;
  f.step = 1;
  f.presetPtr = preset;
  f.strPtr = nullptr;
  f.offset = 0;
  f.tip = tip;
  f.thumbColor = thumbColor;
  f.condition = condition;
  f.page = _currentPage;
  if (_currentPage == -1) _mainHasFields = true;
}

void WebPanel::addConditionalColorPicker(bool (*condition)(),
                                         const String& label, const String& field,
                                         int* preset, const char* tip) {
  ensureFields();
  if (_fieldCount >= _maxFields) return;
  WPField& f = _fields[_fieldCount++];
  f.type = WP_COLORPICKER;
  f.label = label;
  f.fieldName = field;
  f.presetPtr = preset;
  f.strPtr = nullptr;
  f.offset = 0;
  f.tip = tip;
  f.condition = condition;
  f.page = _currentPage;
  if (_currentPage == -1) _mainHasFields = true;
}

// -- New field types -----------------------------------------------------

void WebPanel::addText(const String& label, const String& field,
                               String* ptr, const String& placeholder,
                               const char* tip) {
  ensureFields();
  if (_fieldCount >= _maxFields) return;
  WPField& f = _fields[_fieldCount++];
  f.type = WP_TEXT;
  f.label = label;
  f.fieldName = field;
  f.strPtr = ptr;
  f.presetPtr = nullptr;
  f.extraText = placeholder;
  f.tip = tip;
  f.condition = nullptr;
  f.page = _currentPage;
  if (_currentPage == -1) _mainHasFields = true;
}

void WebPanel::addPassword(const String& label, const String& field, String* ptr,
                            const char* tip) {
  ensureFields();
  if (_fieldCount >= _maxFields) return;
  WPField& f = _fields[_fieldCount++];
  f.type = WP_PASSWORD;
  f.label = label;
  f.fieldName = field;
  f.strPtr = ptr;
  f.presetPtr = nullptr;
  f.tip = tip;
  f.condition = nullptr;
  f.page = _currentPage;
  if (_currentPage == -1) _mainHasFields = true;
}

void WebPanel::addTextInput(const String& label, const String& field, String* ptr,
                             const String& placeholder, int maxLen,
                             const String& buttonLabel, const char* tip,
                             int rows, bool clearable) {
  ensureFields();
  if (_fieldCount >= _maxFields) return;
  WPField& f = _fields[_fieldCount++];
  f.type = WP_TEXT_INPUT;
  f.label = label;
  f.fieldName = field;
  f.strPtr = ptr;
  f.presetPtr = nullptr;
  f.optionsCSV = placeholder;   // repurpose for placeholder text
  f.extraText = buttonLabel;
  f.maxVal = maxLen;
  f.minVal = (rows < 1) ? 1 : rows;   // repurpose: 1 = input, >1 = textarea rows
  f.tip = tip;
  f.clearable = clearable;
  f.condition = nullptr;
  f.page = _currentPage;
  // TextInput has its own Send button — don't trigger Save on home page
}

void WebPanel::addCheckbox(const String& label, const String& field, int* preset,
                            const char* tip) {
  ensureFields();
  if (_fieldCount >= _maxFields) return;
  WPField& f = _fields[_fieldCount++];
  f.type = WP_CHECKBOX;
  f.label = label;
  f.fieldName = field;
  f.presetPtr = preset;
  f.strPtr = nullptr;
  f.tip = tip;
  f.condition = nullptr;
  f.page = _currentPage;
  if (_currentPage == -1) _mainHasFields = true;
}

void WebPanel::addRadio(const String& label, const String& field,
                                const String& options, int* preset,
                                const char* tip) {
  ensureFields();
  if (_fieldCount >= _maxFields) return;
  WPField& f = _fields[_fieldCount++];
  f.type = WP_RADIO;
  f.label = label;
  f.fieldName = field;
  f.presetPtr = preset;
  f.strPtr = nullptr;
  f.offset = 0;
  f.tip = tip;
  f.condition = nullptr;
  f.page = _currentPage;
  f.optionsCSV = options;
  f.optionCount = countOptions(options);
  if (_currentPage == -1) _mainHasFields = true;
}

void WebPanel::addTime(const String& label, const String& field,
                               int* preset, bool includeSeconds,
                               const char* tip) {
  ensureFields();
  if (_fieldCount >= _maxFields) return;
  WPField& f = _fields[_fieldCount++];
  f.type = WP_TIME;
  f.label = label;
  f.fieldName = field;
  f.presetPtr = preset;
  f.strPtr = nullptr;
  f.includeSeconds = includeSeconds;
  f.tip = tip;
  f.condition = nullptr;
  f.page = _currentPage;
  if (_currentPage == -1) _mainHasFields = true;
}

void WebPanel::addNumber(const String& label, const String& field,
                                 int minVal, int maxVal, int step, int* preset,
                                 const char* tip) {
  ensureFields();
  if (_fieldCount >= _maxFields) return;
  WPField& f = _fields[_fieldCount++];
  f.type = WP_NUMBER;
  f.label = label;
  f.fieldName = field;
  f.minVal = minVal;
  f.maxVal = maxVal;
  f.step = step;
  f.presetPtr = preset;
  f.strPtr = nullptr;
  f.tip = tip;
  f.condition = nullptr;
  f.page = _currentPage;
  if (_currentPage == -1) _mainHasFields = true;
}

void WebPanel::addDropDownRange(const String& label, const String& field,
                                        int minVal, int maxVal, int* preset,
                                        const char* tip) {
  ensureFields();
  if (_fieldCount >= _maxFields) return;
  WPField& f = _fields[_fieldCount++];
  f.type = WP_DROPDOWN_RANGE;
  f.label = label;
  f.fieldName = field;
  f.minVal = minVal;
  f.maxVal = maxVal;
  f.presetPtr = preset;
  f.strPtr = nullptr;
  f.tip = tip;
  f.condition = nullptr;
  f.page = _currentPage;
  if (_currentPage == -1) _mainHasFields = true;
}

void WebPanel::addHidden(const String& field, int* preset) {
  ensureFields();
  if (_fieldCount >= _maxFields) return;
  WPField& f = _fields[_fieldCount++];
  f.type = WP_HIDDEN;
  f.fieldName = field;
  f.presetPtr = preset;
  f.strPtr = nullptr;
  f.condition = nullptr;
  f.page = _currentPage;
}

void WebPanel::addHTML(String* htmlPtr) {
  ensureFields();
  if (_fieldCount >= _maxFields) return;
  WPField& f = _fields[_fieldCount++];
  f.type = WP_HTML;
  f.strPtr = htmlPtr;
  f.presetPtr = nullptr;
  f.condition = nullptr;
  f.page = _currentPage;
}

void WebPanel::addButton(const String& label, const String& fieldName, const char* tip) {
  ensureFields();
  if (_fieldCount >= _maxFields) return;
  WPField& f = _fields[_fieldCount++];
  f.type = WP_BUTTON;
  f.label = label;
  f.fieldName = fieldName;
  f.presetPtr = nullptr;
  f.strPtr = nullptr;
  f.tip = tip;
  f.condition = nullptr;
  f.page = _currentPage;
}

// -- Client handling -----------------------------------------------------

void WebPanel::handleClient() {
  if (!_server) return;

  // Drain all pending clients — browsers open multiple parallel connections
  // and ESP32 WiFiServer has a small backlog. Processing only one per call
  // can starve later connections and make the form unresponsive.
  while (_server->hasClient()) {
    WiFiClient client = _server->accept();
    if (!client) break;

    // Force immediate socket reclaim on close (RST instead of TIME_WAIT).
    // ESP32 has ~10 lwIP sockets; without this, TIME_WAIT sockets from
    // prior page loads exhaust the pool after a few navigations.
    struct linger so_linger = { 1, 0 };
    setsockopt(client.fd(), SOL_SOCKET, SO_LINGER, &so_linger, sizeof(so_linger));

    // Disable Nagle's algorithm. Without this, the small headers write
    // followed by the body write gets held by Nagle until the browser's
    // delayed-ACK fires (~40-200 ms), adding that latency to EVERY response
    // — every slider drag, dropdown change and page load. Most painful on
    // no-PSRAM boards where tight lwIP buffering already shrinks the window.
    client.setNoDelay(true);

    // Wait briefly for HTTP request data to arrive after TCP handshake.
    unsigned long waitStart = millis();
    while (!client.available() && client.connected() && millis() - waitStart < 200) {
      delay(1);
    }
    if (!client.available()) { client.stop(); _reqRejected++; continue; }

    client.setTimeout(50);

    // Bounded read of the request line into the static _reqBuf. Replaces
    // String req = readStringUntil('\r') so that we don't pay an alloc/free
    // pair (and a sequence of realloc copies) per request.
    _reqBuf = "";
    {
      unsigned long lastByte = millis();
      while (millis() - lastByte < 50) {
        int c = client.read();
        if (c < 0) { delay(1); continue; }
        lastByte = millis();
        if (c == '\r') break;
        if (_reqBuf.length() < 1024) _reqBuf += (char)c;
      }
    }
    String& req = _reqBuf;  // alias keeps downstream req.indexOf/substring unchanged

    // Read just enough headers for auth check, then discard the rest.
    // Break the instant we see the blank line that ends the headers
    // ("\r\n\r\n"); the form only issues GETs, so there's no body to skip.
    // Previously this loop only exited on a 50 ms idle timeout, adding a
    // hard ~50 ms floor to EVERY request the moment auth was enabled.
    if (_authPass && _authPass->length() > 0) {
      _hdrBuf = "";
      unsigned long lastByte = millis();
      const char eohPat[4] = { '\r', '\n', '\r', '\n' };
      int eoh = 0;  // matched length of the end-of-headers sequence
      while (millis() - lastByte < 50) {
        int c = client.read();
        if (c < 0) {
          if (!client.connected()) break;
          delay(1);
          continue;
        }
        lastByte = millis();
        if (_hdrBuf.length() < 1024) _hdrBuf += (char)c;
        if ((char)c == eohPat[eoh]) {
          if (++eoh == 4) break;  // blank line — end of headers, stop reading
        } else {
          eoh = ((char)c == '\r') ? 1 : 0;
        }
      }
      if (!checkAuth(_hdrBuf)) {
        send401(client);
        continue;
      }
    } else {
      // Drain remaining data without storing it
      while (client.available()) client.read();
    }

    // Minimal diagnostic endpoint. Plain text key=value, stack-local buffer,
    // no allocator dependency — survives even when the 40 KB render buffer
    // can't be allocated (which is exactly when you want to read it).
    if (req.indexOf("GET /health") == 0) {
      handleHealth(client);
      continue;
    }

    // Reject favicon and other non-form requests quickly. Don't count
    // favicon rejects in _reqRejected — iOS Safari requests /favicon.ico
    // on every page load and would otherwise inflate the counter with
    // noise that looks like real failures.
    if (req.indexOf("GET / ") < 0 && req.indexOf("GET /?") < 0 && req.indexOf("GET /page") < 0) {
      if (_captivePortal) {
        // Captive-portal mode (AP/setup): 302-redirect every non-form request
        // (the OS connectivity probes to captive.apple.com / generate_204 /
        // ncsi.txt, plus favicon) to the form root on the AP IP. The phone's
        // probe getting a redirect instead of its expected success response
        // is what triggers the "Sign in to network" popup, which then loads
        // the form. Absolute URL so the captive mini-browser lands on us.
        char redir[128];
        snprintf(redir, sizeof(redir),
          "HTTP/1.1 302 Found\r\nLocation: http://%s/\r\n"
          "Content-Length: 0\r\nConnection: close\r\n\r\n",
          WiFi.softAPIP().toString().c_str());
        client.print(redir);
        client.flush();
        client.stop();
        continue;
      }
      client.print("HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n");
      client.flush();
      client.stop();
      if (req.indexOf("favicon") < 0) _reqRejected++;
      continue;
    }

    if (req.indexOf("/?save=1") >= 0) {
      handleSave(client);
      _reqOK++;
      continue;
    }

    if (req.indexOf("/?field=") >= 0) {
      handleAjax(client, req);
      _reqOK++;
      continue;
    }

    // Determine page from URL
    int page = -1;  // main
    int pageIdx = req.indexOf("GET /page");
    if (pageIdx >= 0) {
      int numStart = pageIdx + 9;
      int spacePos = req.indexOf(' ', numStart);
      if (spacePos > numStart) {
        page = req.substring(numStart, spacePos).toInt();
        if (page < 0 || page >= _numPages) page = -1;
      }
    }

    serveForm(client, page);
    _reqOK++;
  }
}

// Plain-text key=value diagnostic dump. Stack-local buffer, no heap.
// Survives DRAM pressure that would defeat a full form render — which is
// exactly the failure mode we want to be able to read out remotely.
void WebPanel::handleHealth(WiFiClient& client) {
  char body[512];
  long rssi = (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : 0;
  int n = snprintf(body, sizeof(body),
    "freeHeap=%u\n"
    "minFreeHeap=%u\n"
    "maxAllocHeap=%u\n"
    "psramFree=%u\n"
    "rssi=%ld\n"
    "uptimeSec=%lu\n"
    "reqOK=%lu\n"
    "reqRejected=%lu\n"
    "wifi=%s\n"
    "ip=%s\n",
    (unsigned)ESP.getFreeHeap(),
    (unsigned)ESP.getMinFreeHeap(),
    (unsigned)ESP.getMaxAllocHeap(),
    (unsigned)ESP.getFreePsram(),
    rssi,
    (unsigned long)(millis() / 1000UL),
    (unsigned long)_reqOK,
    (unsigned long)_reqRejected,
    (WiFi.status() == WL_CONNECTED) ? "connected" : "disconnected",
    WiFi.localIP().toString().c_str());
  if (n < 0) n = 0;
  if (n >= (int)sizeof(body)) n = sizeof(body) - 1;

  char hdr[192];
  int hn = snprintf(hdr, sizeof(hdr),
    "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: %d\r\n"
    "Cache-Control: no-store\r\nConnection: close\r\n\r\n", n);
  if (hn < 0) hn = 0;
  if (hn >= (int)sizeof(hdr)) hn = sizeof(hdr) - 1;
  client.write((const uint8_t*)hdr, hn);
  client.write((const uint8_t*)body, n);
  client.flush();
  client.stop();
  _reqOK++;
}

void WebPanel::sendOK(WiFiClient& client) {
  // Use the pending message (set by showMessage()) if non-empty,
  // otherwise return "OK". Always clear the pending message after.
  const char* body;
  int bodyLen;
  if (_pendingMessage.length() > 0) {
    body = _pendingMessage.c_str();
    bodyLen = _pendingMessage.length();
  } else {
    body = "OK";
    bodyLen = 2;
  }
  char headers[128];
  snprintf(headers, sizeof(headers),
    "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: %d\r\nConnection: close\r\n\r\n",
    bodyLen);
  client.print(headers);
  client.write((const uint8_t*)body, bodyLen);
  client.flush();
  client.stop();
  _pendingMessage = "";  // clear after sending
}

bool WebPanel::checkAuth(const String& hdrs) {
  int idx = hdrs.indexOf("Authorization: Basic ");
  if (idx < 0) return false;
  int start = idx + 21;
  int end = hdrs.indexOf('\r', start);
  if (end < 0) end = hdrs.indexOf('\n', start);
  if (end < 0) end = hdrs.length();
  String encoded = hdrs.substring(start, end);
  encoded.trim();

  // Base64-decode the credential
  size_t decLen = 0;
  unsigned char dec[128];
  int ret = mbedtls_base64_decode(dec, sizeof(dec) - 1, &decLen,
              (const unsigned char*)encoded.c_str(), encoded.length());
  if (ret != 0) return false;
  dec[decLen] = '\0';

  // Format is "user:password" — extract password after ':'
  const char* colon = strchr((const char*)dec, ':');
  if (!colon) return false;
  return *_authPass == (colon + 1);
}

void WebPanel::send401(WiFiClient& client) {
  client.print(
    "HTTP/1.1 401 Unauthorized\r\n"
    "WWW-Authenticate: Basic realm=\"Settings\"\r\n"
    "Content-Length: 0\r\n"
    "Connection: close\r\n\r\n");
  client.flush();
  client.stop();
}

void WebPanel::handleSave(WiFiClient& client) {
  if (_saveCb) _saveCb();
  sendOK(client);
}

void WebPanel::handleAjax(WiFiClient& client, const String& req) {
  int fi  = req.indexOf("field=")  + 6;
  int vi  = req.indexOf("value=");
  int fe  = req.indexOf('&', fi);
  int fe2 = req.indexOf(' ', fi);
  if (fe < 0 || fe2 < fe) fe = fe2;
  String field = req.substring(fi, fe);
  String value = "";
  if (vi >= 0) {
    int vs = vi + 6;
    int ve = req.indexOf(' ', vs);
    if (ve < 0) ve = req.length();
    value  = req.substring(vs, ve);
    // Strip any trailing parameters
    int amp = value.indexOf('&');
    if (amp >= 0) value = value.substring(0, amp);
  }

  // Find matching field
  for (int i = 0; i < _fieldCount; i++) {
    WPField& f = _fields[i];
    if (f.fieldName != field) continue;

    // Text-based fields: update String pointer
    if (f.type == WP_TEXT || f.type == WP_PASSWORD || f.type == WP_TEXT_INPUT) {
      String decoded = urlDecode(value);
      if (f.strPtr) *f.strPtr = decoded;
      if (_textCb) _textCb(field, decoded);
      sendOK(client);
      return;
    }

    // Integer-based fields
    int v = value.toInt();

    if (f.type == WP_TIME) {
      // Value comes as HHMM integer from JS
      if (f.presetPtr) *f.presetPtr = v;
    }
    else if (f.type == WP_CHECKBOX) {
      if (f.presetPtr) *f.presetPtr = (v != 0) ? 1 : 0;
    }
    else if (f.type == WP_RANGE || f.type == WP_NUMBER) {
      if (f.presetPtr) *f.presetPtr = constrain(v, f.minVal, f.maxVal);
    }
    else if (f.type == WP_DROPDOWN_OFFSET) {
      if (f.presetPtr) *f.presetPtr = constrain(v, f.offset, f.optionCount - 1 + f.offset);
    }
    else if (f.type == WP_DROPDOWN || f.type == WP_RADIO) {
      if (f.presetPtr) *f.presetPtr = constrain(v, 0, f.optionCount - 1);
    }
    else if (f.type == WP_DROPDOWN_RANGE) {
      if (f.presetPtr) *f.presetPtr = constrain(v, f.minVal, f.maxVal);
    }
    else if (f.type == WP_COLORPICKER) {
      if (f.presetPtr) *f.presetPtr = v;
    }
    else if (f.type == WP_HIDDEN) {
      if (f.presetPtr) *f.presetPtr = v;
    }
    else if (f.type == WP_BUTTON) {
      // Stateless — no preset to update; just fall through to the callback.
    }

    if (_changeCb) _changeCb(field, v);
    break;
  }

  sendOK(client);
}

// -- HTML generators -----------------------------------------------------

void WebPanel::genDropDown(int idx) {
  WPField& f = _fields[idx];
  int sel = f.presetPtr ? *f.presetPtr : 0;

  String opts[WP_MAX_OPTIONS];
  int count;
  parseOptions(f.optionsCSV, opts, count);

  out("<div class=\"fg\"><label class=\"fl\">");
  out(f.label);
  emitTipIcon(idx);
  out("</label>");
  emitTipBox(idx);
  out("<select id=\"");
  out(f.fieldName);
  out("\" onchange=\"send('");
  out(f.fieldName);
  out("',this.value)\">");

  for (int i = 0; i < count; i++) {
    int val = i + f.offset;
    out("<option value=\"");
    out(val);
    out("\"");
    if (val == sel) out(" selected");
    out(">");
    out(opts[i]);
    out("</option>");
  }
  out("</select></div>");
}

void WebPanel::genRange(int idx) {
  WPField& f = _fields[idx];
  int val = f.presetPtr ? *f.presetPtr : f.minVal;

  out("<div class=\"fg\"><label class=\"fl\">");
  out(f.label);
  emitTipIcon(idx);
  out("</label>");
  emitTipBox(idx);
  out("<div class=\"rc\">");
  out("<input type=\"range\" min=\""); out(f.minVal);
  out("\" max=\""); out(f.maxVal);
  out("\" value=\""); out(val);
  out("\" id=\""); out(f.fieldName);
  if (f.thumbColor) {
    out("\" data-tc=\""); out(f.thumbColor);
  }
  // Slider value send: use debounced oninput, not onchange. iOS Safari has
  // a known quirk where the change event on <input type=range> doesn't fire
  // reliably after a drag-release (it does fire when the track is tapped).
  // Switching to oninput + a 150 ms debouncer fires regardless, and the
  // single-flight AbortController in _safeFetch coalesces rapid drags.
  out("\" oninput=\"document.getElementById('"); out(f.fieldName);
  out("_v').textContent=this.value;stc(this);rng(this)\">");
  out("<span class=\"rv\" id=\""); out(f.fieldName); out("_v\">");
  out(val); out("</span>");
  out("</div></div>");
}

void WebPanel::genSubheading(int idx) {
  out("<h2 class=\"sh\">");
  out(_fields[idx].label);
  out("</h2>");
}

void WebPanel::genSeparator() {
  out("<div class=\"sep\"></div>");
}

void WebPanel::genColorPicker(int idx) {
  WPField& f = _fields[idx];
  int val = f.presetPtr ? *f.presetPtr : 0;
  char hex[8];
  snprintf(hex, sizeof(hex), "#%06X", val & 0xFFFFFF);

  out("<div class=\"fg\"><label class=\"fl\">");
  out(f.label);
  emitTipIcon(idx);
  out("</label>");
  emitTipBox(idx);
  out("<input type=\"color\" id=\"");
  out(f.fieldName);
  out("\" value=\"");
  out(hex);
  // oninput drives live preview during selection (Edge keeps the picker
  // open until commit; relying on onchange alone makes the LED stall
  // until the user closes the dialog). onchange is kept as a safety net
  // for any browser that doesn't fire oninput on a color input. clr()
  // debounces by 150 ms so a single drag doesn't flood the server.
  out("\" oninput=\"clr(this)\" onchange=\"clr(this)\"></div>");
}

void WebPanel::genText(int idx) {
  WPField& f = _fields[idx];
  const char* val = f.strPtr ? f.strPtr->c_str() : "";

  out("<div class=\"fg\"><label class=\"fl\">");
  out(f.label);
  emitTipIcon(idx);
  out("</label>");
  emitTipBox(idx);
  out("<input type=\"text\" id=\"");
  out(f.fieldName);
  out("\" value=\"");
  out(val);
  out("\"");
  if (f.extraText.length() > 0) {
    out(" placeholder=\"");
    out(f.extraText);
    out("\"");
  }
  out(" onblur=\"sendStr('");
  out(f.fieldName);
  out("',this.value)\" onkeydown=\"if(event.key==='Enter'){sendStr('");
  out(f.fieldName);
  out("',this.value);this.blur();}\">");
  out("</div>");
}

void WebPanel::genTextInput(int idx) {
  WPField& f = _fields[idx];
  const char* val = f.strPtr ? f.strPtr->c_str() : "";
  int rows = (f.minVal < 1) ? 1 : f.minVal;
  bool isArea = rows > 1;

  out("<div class=\"fg\"><label class=\"fl\">");
  out(f.label);
  emitTipIcon(idx);
  out("</label>");
  emitTipBox(idx);
  // Multi-line mode: stack textarea + button vertically, both full-width.
  if (isArea) {
    out("<div class=\"tr\" style=\"display:block\">");
  } else {
    out("<div class=\"tr\">");
  }
  // Single-line + clearable: wrap the input in a position:relative div so
  // an absolutely-positioned "x" can sit on the input's right edge (iOS /
  // Safari search-field style). Only shown when the input has content.
  bool wrapClear = !isArea && f.clearable;
  if (wrapClear) out("<div class=\"ti-wrap\">");
  if (isArea) {
    out("<textarea class=\"ta\" id=\"");
    out(f.fieldName);
    out("\" rows=\"");
    out(rows);
    out("\"");
  } else {
    out("<input type=\"text\" id=\"");
    out(f.fieldName);
    out("\" value=\"");
    out(val);
    out("\"");
    if (wrapClear) out(" class=\"ti-x\"");
  }
  if (f.maxVal > 0) {
    out(" maxlength=\"");
    out(f.maxVal);
    out("\"");
  }
  if (f.optionsCSV.length() > 0) {
    out(" placeholder=\"");
    out(f.optionsCSV);
    out("\"");
  }
  out(" autocomplete=\"off\" autocorrect=\"off\" autocapitalize=\"off\" spellcheck=\"false\"");
  if (isArea) {
    out(">");
    out(val);                   // textarea content goes between tags
    out("</textarea>");
  } else if (wrapClear) {
    // oninput toggles the X visibility based on value
    out(" oninput=\"document.getElementById('");
    out(f.fieldName);
    out("_x').style.visibility=this.value?'visible':'hidden'\"");
    out(" onkeydown=\"if(event.key==='Enter')sendText('");
    out(f.fieldName);
    out("')\">");
    // The X — initial display=none if empty, default visible if has value
    out("<span class=\"cx\" id=\"");
    out(f.fieldName);
    out("_x\" style=\"visibility:");
    out((val && val[0]) ? "visible" : "hidden");
    out("\" onclick=\"var e=document.getElementById('");
    out(f.fieldName);
    out("');e.value='';this.style.visibility='hidden';sendStr('");
    out(f.fieldName);
    out("','')\">\xC3\x97</span>");   // UTF-8 multiplication sign (×)
  } else {
    out(" onkeydown=\"if(event.key==='Enter')sendText('");
    out(f.fieldName);
    out("')\">");
  }
  if (wrapClear) out("</div>");
  if (isArea) {
    out("<button class=\"sb sb-block\" onclick=\"sendText('");
  } else {
    out("<button class=\"sb\" onclick=\"sendText('");
  }
  out(f.fieldName);
  out("')\">");
  out(f.extraText);
  out("</button></div></div>");
}

void WebPanel::genHTML(int idx) {
  WPField& f = _fields[idx];
  if (f.strPtr) out(*f.strPtr);
}

void WebPanel::genButton(int idx) {
  WPField& f = _fields[idx];
  out("<button class=\"sb sb-block\" onclick=\"send('");
  out(f.fieldName);
  out("',1)\">");
  out(f.label);
  out("</button>");
  emitTipBox(idx);
}

void WebPanel::genPassword(int idx) {
  WPField& f = _fields[idx];
  const char* val = f.strPtr ? f.strPtr->c_str() : "";

  out("<div class=\"fg\"><label class=\"fl\">");
  out(f.label);
  emitTipIcon(idx);
  out("</label>");
  emitTipBox(idx);
  out("<div class=\"pw\">");
  // autocomplete="off" + the iOS-specific opt-outs suppress Safari's
  // "Use Strong Password" / iCloud Keychain suggestion popup. spellcheck
  // and autocorrect off prevent the on-screen keyboard from offering
  // typo suggestions over the password.
  out("<input type=\"password\" id=\"");
  out(f.fieldName);
  out("\" value=\"");
  out(val);
  out("\" autocomplete=\"off\" autocorrect=\"off\" autocapitalize=\"none\" spellcheck=\"false\"");
  out(" onblur=\"sendStr('");
  out(f.fieldName);
  out("',this.value)\" onkeydown=\"if(event.key==='Enter'){sendStr('");
  out(f.fieldName);
  out("',this.value);this.blur();}\">");
  out("<label class=\"sp\"><input type=\"checkbox\" onclick=\"var p=document.getElementById('");
  out(f.fieldName);
  out("');p.type=p.type==='password'?'text':'password';\"><span>Show</span></label>");
  out("</div></div>");
}

void WebPanel::genCheckbox(int idx) {
  WPField& f = _fields[idx];
  int val = f.presetPtr ? *f.presetPtr : 0;

  out("<div class=\"fg cb-group\"><label class=\"cb-label\">");
  out("<input type=\"checkbox\" id=\"");
  out(f.fieldName);
  out("\"");
  if (val) out(" checked");
  out(" onchange=\"send('");
  out(f.fieldName);
  out("',this.checked?1:0)\">");
  out("<span class=\"cb-text\">");
  out(f.label);
  out("</span>");
  // Tip icon goes inside the label so it's positioned next to the text.
  // The icon's onclick uses event.stopPropagation to prevent the checkbox
  // from toggling when the icon is tapped.
  emitTipIcon(idx);
  out("</label>");
  emitTipBox(idx);
  out("</div>");
}

void WebPanel::genRadio(int idx) {
  WPField& f = _fields[idx];
  int sel = f.presetPtr ? *f.presetPtr : 0;

  String opts[WP_MAX_OPTIONS];
  int count;
  parseOptions(f.optionsCSV, opts, count);

  out("<div class=\"fg\"><label class=\"fl\">");
  out(f.label);
  emitTipIcon(idx);
  out("</label>");
  emitTipBox(idx);

  for (int i = 0; i < count; i++) {
    out("<div class=\"rd-group\"><label class=\"rd-label\">");
    out("<input type=\"radio\" name=\"");
    out(f.fieldName);
    out("\" value=\"");
    out(i);
    out("\"");
    if (i == sel) out(" checked");
    out(" onchange=\"send('");
    out(f.fieldName);
    out("',this.value)\">");
    out("<span class=\"rd-text\">");
    out(opts[i]);
    out("</span></label></div>");
  }
  out("</div>");
}

void WebPanel::genTime(int idx) {
  WPField& f = _fields[idx];
  int val = f.presetPtr ? *f.presetPtr : 0;
  int hours = val / 100;
  int minutes = val % 100;
  char timeBuf[6];
  snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d", hours, minutes);

  out("<div class=\"fg\"><label class=\"fl\">");
  out(f.label);
  emitTipIcon(idx);
  out("</label>");
  emitTipBox(idx);
  out("<input type=\"time\" value=\"");
  out(timeBuf);
  out("\"");
  if (f.includeSeconds) out(" step=\"1\"");
  out(" onblur=\"var p=this.value.split(':');send('");
  out(f.fieldName);
  out("',parseInt(p[0])*100+parseInt(p[1]))\">");
  out("</div>");
}

void WebPanel::genNumber(int idx) {
  WPField& f = _fields[idx];
  int val = f.presetPtr ? *f.presetPtr : f.minVal;

  out("<div class=\"fg\"><label class=\"fl\">");
  out(f.label);
  emitTipIcon(idx);
  out("</label>");
  emitTipBox(idx);
  out("<input type=\"number\" min=\"");
  out(f.minVal);
  out("\" max=\"");
  out(f.maxVal);
  out("\" step=\"");
  out(f.step);
  out("\" value=\"");
  out(val);
  out("\" onchange=\"send('");
  out(f.fieldName);
  out("',this.value)\">");
  out("</div>");
}

void WebPanel::genDropDownRange(int idx) {
  WPField& f = _fields[idx];
  int sel = f.presetPtr ? *f.presetPtr : f.minVal;

  out("<div class=\"fg\"><label class=\"fl\">");
  out(f.label);
  emitTipIcon(idx);
  out("</label>");
  emitTipBox(idx);
  out("<select id=\"");
  out(f.fieldName);
  out("\" onchange=\"send('");
  out(f.fieldName);
  out("',this.value)\">");

  for (int v = f.minVal; v <= f.maxVal; v++) {
    out("<option value=\"");
    out(v);
    out("\"");
    if (v == sel) out(" selected");
    out(">");
    out(v);
    out("</option>");
  }
  out("</select></div>");
}

void WebPanel::genHidden(int idx) {
  WPField& f = _fields[idx];
  int val = f.presetPtr ? *f.presetPtr : 0;
  out("<input type=\"hidden\" id=\"");
  out(f.fieldName);
  out("\" value=\"");
  out(val);
  out("\">");
}

void WebPanel::genPageButton(int idx) {
  WPField& f = _fields[idx];
  out("<a href=\"/page");
  out(f.offset);  // page index
  out("\" class=\"page-btn\"");
  if (f.thumbColor != nullptr) {
    // Optional per-button background color override (addPage's buttonColor arg).
    out(" style=\"background:");
    out(f.thumbColor);
    out(";\"");
  }
  out(">");
  out(f.label);
  out("</a>");
}

void WebPanel::genActionButton(int idx) {
  WPField& f = _fields[idx];
  out("<button type=\"button\" class=\"action-btn\"");
  if (f.extraText.length() > 0) {
    // Confirm-and-clear mode: fire-and-forget AJAX, then replace whole body
    // with the confirmation message overlay. Used for actions that reboot
    // the device (response will never arrive anyway).
    out(" data-msg=\"");
    out(f.extraText);
    out("\"");
    if (f.reloadAfter) out(" data-reload=\"1\"");
    if (f.reloadAfter && f.optionsCSV.length() > 0) {
      out(" data-statusfield=\"");
      out(f.optionsCSV);
      out("\"");
    }
    out(" onclick=\"actionClear('");
    out(f.fieldName);
    out("',this)\"");
  } else {
    // Standard mode: send field change, callback can show validation errors
    out(" onclick=\"send('");
    out(f.fieldName);
    out("',1)\"");
  }
  out(">");
  out(f.label);
  out("</button>");
}

// -- Tooltip helpers -----------------------------------------------------
// Emit the info icon (ⓘ) inside the label text. No-op if tip empty.
// stopPropagation stops DOM event bubbling. preventDefault cancels the
// browser's separate "label-click toggles the associated input" code path
// — without it, tapping the ⓘ inside a checkbox <label> would still flip
// the checkbox state.
void WebPanel::emitTipIcon(int idx) {
  WPField& f = _fields[idx];
  if (!f.tip || f.tip[0] == '\0') return;
  out(" <span class=\"info\" onclick=\"event.preventDefault();event.stopPropagation();tipToggle('");
  out(f.fieldName);
  out("',this)\">&#9432;</span>");
}

// Emit the floating tooltip bubble div. No-op if tip empty.
// Outer .tt is the sized bubble (flex column); inner .tt-i is the
// scrollable content area. See the .tt CSS for the Edge bug rationale.
void WebPanel::emitTipBox(int idx) {
  WPField& f = _fields[idx];
  if (!f.tip || f.tip[0] == '\0') return;
  out("<div class=\"tt\" id=\"tt_");
  out(f.fieldName);
  out("\"><div class=\"tt-i\">");
  out(f.tip);
  out("</div></div>");
}

// -- Form rendering ------------------------------------------------------

void WebPanel::serveForm(WiFiClient& client, int page) {
  // The render buffer may be null here: freeBuffer() is called before an OTA
  // TLS handshake to reclaim heap, and a subsequent allocBuffer() can fail to
  // re-acquire a large contiguous block on a fragmented/low (no-PSRAM) heap.
  // Try to (re)allocate; if it's still null, serve a tiny auto-refreshing page
  // instead of writing through null below (which faults as StoreProhibited).
  // This self-heals: once the heap recovers (e.g. the OTA TLS client is torn
  // down), the next poll's allocBuffer() succeeds and the full form renders.
  if (!_htmlBuf) allocBuffer();
  if (!_htmlBuf) {
    const char* body = "<!DOCTYPE html><meta http-equiv=\"refresh\" content=\"3\">"
                       "<body style=\"font-family:sans-serif\">Low memory \xE2\x80\x94 retrying\xE2\x80\xA6</body>";
    int blen = strlen(body);
    char headers[160];
    int hn = snprintf(headers, sizeof(headers),
      "HTTP/1.1 503 Service Unavailable\r\nContent-Type: text/html\r\n"
      "Content-Length: %d\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n", blen);
    client.write((const uint8_t*)headers, hn);
    client.write((const uint8_t*)body, blen);
    client.flush();
    client.stop();
    return;
  }

  // Reset the static buffer
  _htmlPos = 0;
  _htmlBuf[0] = 0;

  // Determine header lines for the requested page (home or sub-page)
  String displayLine1 = _titleLine1;
  String displayLine2 = _titleLine2;
  if (page >= 0 && page < _numPages) {
    displayLine1 = _pageLine1[page];
    displayLine2 = _pageLine2[page];
  }
  // Browser title bar uses line1 only — line2 may contain HTML for the
  // in-page header, which <title> renders as literal text and browsers
  // cache into history/autocomplete.
  String docTitle = displayLine1;

  out("<!DOCTYPE html><html lang=\"en\"><head>");
  out("<meta charset=\"UTF-8\">");
  out("<meta name=\"viewport\" content=\"width=device-width,initial-scale=1.0\">");
  out("<title>"); out(docTitle); out("</title>");
  out("<style>");

  // -- Design tokens --
  out(":root{");
  out("--pc:#3b82f6;--ph:#2563eb;--pd:#1d4ed8;");
  out("--sc:#10b981;--sh:#059669;");
  out("--bg:#f1f5f9;--cb:#ffffff;");
  out("--tp:#0f172a;--ts:#64748b;--tl:#94a3b8;");
  out("--br:#e2e8f0;--bf:#3b82f6;");
  out("--r:12px;--ri:10px;");
  // NOTE: 'thumb' name (not 'ts') because --ts is already used for
  // text-secondary color. Reusing --ts here made the thumb width
  // collapse to an invalid value in dark mode where --ts is set to a
  // color, causing sliders to be ungrabbable on iPhones in dark mode.
  out("--th:"); out(_sliderTrack); out("px;--thumb:"); out(_sliderThumb); out("px;");
  out("--sh-sm:0 1px 3px rgba(0,0,0,.08);");
  out("--sh-md:0 4px 12px rgba(0,0,0,.1);");
  out("--sh-lg:0 12px 28px rgba(0,0,0,.12);");
  out("--mono:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;");
  // Tooltip surface (GitHub convention: dark in light mode, slightly lighter
  // dark in dark mode so the bubble pops off the already-dark page).
  out("--tt-bg:#0f172a;--tt-fg:#ffffff;");
  out("}");
  // -- Dark mode — redefine the design tokens. Triggers automatically when
  //    the OS / browser is set to dark.
  out("@media (prefers-color-scheme:dark){:root{");
  out("--bg:#0f172a;--cb:#1e293b;");
  out("--tp:#e2e8f0;--ts:#94a3b8;--tl:#64748b;");
  out("--br:#334155;");
  out("--tt-bg:#475569;--tt-fg:#ffffff;");
  out("--sh-sm:0 1px 3px rgba(0,0,0,.4);");
  out("--sh-md:0 4px 12px rgba(0,0,0,.5);");
  out("--sh-lg:0 12px 28px rgba(0,0,0,.6);");
  out("}}");

  // -- Reset & base --
  out("*{box-sizing:border-box;margin:0;padding:0;}");
  out("html,body{overflow-x:hidden;max-width:100%;}");
  // Stop the rubber-band over-scroll past the page's edges (both axes) so long
  // pages (e.g. the Rule Editor) can't be flung off-screen vertically or panned
  // sideways — they lock at their natural boundaries like the short pages. (iOS
  // Safari support for this on the root scroller is version-dependent; harmless
  // where unsupported.)
  out("body{overscroll-behavior:none;}");
  // Bump the root font-size so all rem-based text scales up ~6%. Container
  // max-width stays in px so the form's width is unchanged; only text grows.
  out("html{font-size:17px;}");
  out("body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,'Helvetica Neue',sans-serif;");
  out("background:var(--bg);color:var(--tp);line-height:1.6;padding:16px;");
  out("-webkit-font-smoothing:antialiased;-moz-osx-font-smoothing:grayscale;}");

  // -- Container --
  // Clip HORIZONTAL overflow here so a child a few px wider than the viewport
  // (a width:100% field + padding, or the iOS time input) can't pan the whole
  // page sideways — iOS Safari doesn't reliably honor overflow-x:hidden on the
  // root/body scroller, but does on a normal element like this. overflow-x:clip
  // keeps overflow-y visible (no scroll container) so vertical page scroll is
  // unchanged; the plain overflow-x:hidden is a fallback for browsers without
  // clip. Tooltips are unaffected: open ones are position:fixed (escape the
  // clip) and closed ones are collapsed to zero size.
  out("#container{max-width:640px;margin:0 auto;background:var(--cb);");
  out("border-radius:var(--r);box-shadow:var(--sh-lg);overflow-x:hidden;overflow-x:clip;}");

  // -- Header --
  out("#header{background:linear-gradient(135deg,var(--pc),var(--pd));");
  out("color:#fff;text-align:center;font-size:clamp(1.1rem,5vw,1.7rem);font-weight:700;");
  out("padding:8px 24px;letter-spacing:-.3px;line-height:1.1;white-space:nowrap;");
  out("border-radius:var(--r) var(--r) 0 0;}");
  out("#header .hl2{display:block;font-size:clamp(0.9rem,4vw,1.4rem);font-weight:500;");
  out("opacity:0.92;margin-top:1px;letter-spacing:0;line-height:1.1;}");

  // -- Form area --
  out("#inputs{padding:24px;}");

  // -- Field group --
  out(".fg{margin-bottom:20px;position:relative;}");
  out(".fl{display:block;font-size:.9rem;font-weight:600;color:var(--ts);");
  out("text-transform:uppercase;letter-spacing:.5px;margin-bottom:6px;}");

  // -- Tooltip info icon and dark bubble tooltip --
  out(".info{display:inline-block;margin-left:6px;font-size:1.1rem;");
  out("color:var(--pc);cursor:pointer;user-select:none;line-height:1;}");
  // Dark slate bubble — portal'd to <body> and positioned via JS (position:
  // fixed, centered horizontally, vertical anchored to icon). Outer .tt is
  // the flex container; inner .tt-i is the scrollable region with the
  // scrollbar hidden (the wheel handler drives the scroll explicitly,
  // which also works around an Edge bug where wheel events over an
  // overflow:auto element sometimes route to the page underneath).
  // position:absolute (default) keeps closed tooltips out of document flow;
  // JS overrides to position:fixed when opening to portal them into body.
  out(".tt{position:absolute;box-sizing:border-box;z-index:100;");
  out("background:var(--tt-bg);color:var(--tt-fg);border:none;border-radius:12px;");
  out("font-size:.9rem;font-weight:400;line-height:1.4;");
  out("text-transform:none;letter-spacing:0;");
  out("box-shadow:0 6px 16px rgba(0,0,0,.25);cursor:pointer;");
  out("display:flex;flex-direction:column;");
  out("opacity:0;visibility:hidden;");
  out("transition:opacity .15s ease,visibility .15s;pointer-events:none;}");
  out(".tt.open{opacity:1;visibility:visible;pointer-events:auto;}");
  // Collapse CLOSED tooltips to zero box height. A closed .tt is position:
  // absolute and invisible, but its box still extends the page's scrollable
  // height — and a large tip (e.g. the Rule Editor's full schema doc) creates
  // a big invisible scroll dead-zone you can scroll the form into / off. Only
  // closed tips are collapsed; open tips are position:fixed (set by tipToggle)
  // and unaffected by :not(.open), so this changes nothing about how tips show.
  out(".tt:not(.open){max-height:0;overflow:hidden;}");
  out(".tt-i{flex:1 1 auto;min-height:0;overflow-y:auto;overflow-x:hidden;");
  out("padding:10px 14px;overflow-wrap:anywhere;word-break:break-word;");
  out("overscroll-behavior:contain;scrollbar-width:none;}");
  out(".tt-i::-webkit-scrollbar{width:0;height:0;background:transparent;}");

  // -- Inputs (shared) --
  out("select,input[type=text],input[type=password],input[type=number],input[type=time]{");
  out("width:100%;height:48px;padding:10px 14px;font-size:1rem;font-family:inherit;");
  out("border:1.5px solid var(--br);border-radius:var(--ri);background:var(--cb);");
  out("outline:none;color:var(--tp);transition:border-color .15s,box-shadow .15s;}");
  // iOS Safari time input fix — native rendering adds internal padding that
  // ignores box-sizing and overflows the container.
  out("input[type=time]{-webkit-appearance:none;appearance:none;");
  out("min-width:0;max-width:100%;box-sizing:border-box;}");
  out("select:focus,input[type=text]:focus,input[type=password]:focus,");
  out("input[type=number]:focus,input[type=time]:focus{");
  out("border-color:var(--bf);box-shadow:0 0 0 3px rgba(59,130,246,.15);}");

  // -- Range slider (cross-browser) --
  out(".rc{display:flex;align-items:center;gap:14px;}");
  out("input[type=range]{flex:1;height:var(--th);appearance:none;-webkit-appearance:none;");
  out("background:var(--br);border-radius:calc(var(--th)/2);outline:none;cursor:pointer;}");
  out("input[type=range]::-webkit-slider-thumb{appearance:none;-webkit-appearance:none;");
  out("width:var(--thumb);height:var(--thumb);background:var(--tc,var(--pc));border-radius:50%;cursor:pointer;");
  out("box-shadow:0 1px 4px rgba(0,0,0,.2);transition:transform .15s,background .1s;}");
  out("input[type=range]::-moz-range-thumb{width:var(--thumb);height:var(--thumb);background:var(--tc,var(--pc));");
  out("border-radius:50%;border:none;cursor:pointer;box-shadow:0 1px 4px rgba(0,0,0,.2);}");
  out("input[type=range]:active::-webkit-slider-thumb{transform:scale(1.15);}");
  out("input[type=range]:active::-moz-range-thumb{transform:scale(1.15);}");
  out(".rv{background:var(--tc,var(--pc));color:#fff;padding:2px 10px;border-radius:8px;");
  out("font-size:.85rem;font-weight:700;min-width:32px;text-align:center;transition:background .1s;}");

  // -- Color picker (cross-browser) --
  out("input[type=color]{width:100%;height:48px;padding:3px;border:1.5px solid var(--br);");
  out("border-radius:var(--ri);cursor:pointer;appearance:none;-webkit-appearance:none;");
  out("background:var(--cb);transition:border-color .15s;}");
  out("input[type=color]::-webkit-color-swatch-wrapper{padding:0;}");
  out("input[type=color]::-webkit-color-swatch{border:none;border-radius:7px;}");
  out("input[type=color]::-moz-color-swatch{border:none;border-radius:7px;}");
  out("input[type=color]:focus,input[type=color]:hover{border-color:var(--bf);}");

  // -- Subheading --
  out(".sh{font-size:1.1rem;font-weight:700;color:var(--tp);");
  out("margin:28px 0 16px 0;padding-bottom:8px;border-bottom:2px solid var(--br);}");
  out(".sh:first-child{margin-top:0;}");

  // -- Separator --
  out(".sep{width:100%;height:1px;background:var(--br);margin:24px 0;}");

  // -- Text input with button --
  out(".tr{display:flex;gap:8px;align-items:center;}");
  out(".tr input[type=text]{flex:1;}");
  out(".sb{height:48px;padding:0 18px;font-size:1.05rem;font-weight:600;color:#fff;font-family:inherit;");
  out("background:var(--pc);border:none;border-radius:var(--ri);cursor:pointer;");
  out("white-space:nowrap;transition:background .15s;}");
  out(".sb:active{background:var(--pd);}");
  // -- Full-width button modifier (used by addButton and addTextInput's
  //    Apply button when in textarea mode). Class-based so the buttons in
  //    those flows don't carry inline style attributes.
  out(".sb-block{width:100%;display:block;margin:10px 0;}");
  // -- Inline clear "x" for addTextInput (clearable=true). The input is
  //    wrapped in .ti-wrap (position:relative) so .cx can sit absolutely
  //    on the input's right edge. .ti-x adds padding-right on the input
  //    so the typed text doesn't run under the X. iOS/Safari clear-field
  //    convention: subtle gray circle, only visible when the input has
  //    content (toggled by the oninput handler).
  out(".ti-wrap{position:relative;flex:1;min-width:0;display:flex;align-items:center;}");
  out(".tr input[type=text].ti-x{padding-right:38px;}");
  out(".cx{position:absolute;right:10px;top:50%;transform:translateY(-50%);");
  out("color:var(--tl);font-size:1.25rem;line-height:1;cursor:pointer;");
  out("user-select:none;-webkit-tap-highlight-color:transparent;");
  out("padding:4px;transition:color .15s;}");
  out(".cx:hover{color:var(--ts);}");
  out(".cx:active{color:var(--tp);}");
  // -- Textarea (multi-line input, rendered when rows>1 in addTextInput) --
  out(".ta{width:100%;font-family:var(--mono);font-size:14px;padding:8px;");
  out("border:1.5px solid var(--br);border-radius:var(--ri);background:var(--cb);");
  out("color:var(--tp);resize:vertical;box-sizing:border-box;outline:none;");
  out("transition:border-color .15s,box-shadow .15s;}");
  out(".ta:focus{border-color:var(--bf);box-shadow:0 0 0 3px rgba(59,130,246,.15);}");

  // -- Password --
  out(".pw{display:flex;align-items:center;gap:10px;}");
  out(".pw input[type=password],.pw input[type=text]{flex:1;}");
  out(".sp{display:flex;align-items:center;cursor:pointer;font-size:.8rem;");
  out("color:var(--ts);white-space:nowrap;user-select:none;}");
  out(".sp input[type=checkbox]{width:16px;height:16px;margin-right:5px;cursor:pointer;}");

  // -- Checkbox --
  out(".cb-group{margin-bottom:8px;}");
  // width:fit-content shrinks the label to just its checkbox + text + icon so
  // clicks in the empty space to the right of the row don't toggle the input.
  out(".cb-label{display:flex;align-items:center;cursor:pointer;width:fit-content;max-width:100%;");
  out("font-size:1rem;font-weight:500;color:var(--tp);padding:6px 0;min-height:44px;}");
  out(".cb-label input[type=checkbox]{width:22px;height:22px;margin-right:12px;");
  out("cursor:pointer;accent-color:var(--pc);}");

  // -- Radio --
  out(".rd-group{margin-bottom:4px;}");
  out(".rd-label{display:flex;align-items:center;cursor:pointer;");
  out("font-size:1rem;font-weight:500;color:var(--tp);padding:6px 0;min-height:44px;}");
  out(".rd-label input[type=radio]{width:22px;height:22px;margin-right:12px;");
  out("cursor:pointer;accent-color:var(--pc);}");

  // -- Page nav button --
  out(".page-btn{display:block;width:100%;padding:12px;font-size:1.05rem;font-weight:600;");
  out("color:#fff;background:var(--pc);border:none;border-radius:var(--r);");
  out("cursor:pointer;text-align:center;text-decoration:none;");
  out("transition:background .15s,box-shadow .15s;margin-bottom:10px;box-shadow:var(--sh-sm);}");
  out(".page-btn:active{background:var(--pd);}");

  // -- Back button --
  out(".back-btn{display:block;width:100%;padding:12px;font-size:1.05rem;font-weight:600;");
  out("color:var(--ts);background:var(--bg);border:1.5px solid var(--br);");
  out("border-radius:var(--r);cursor:pointer;text-align:center;text-decoration:none;");
  out("transition:border-color .15s,color .15s;margin-bottom:10px;}");
  out(".back-btn:active{border-color:var(--ts);color:var(--tp);}");

  // -- Text input with send button --
  out(".tr{display:flex;gap:10px;align-items:center;}");
  out(".tr input[type=text]{flex:1;height:48px;padding:12px;font-size:1.1rem;");
  out("border:2px solid var(--br);border-radius:8px;background:var(--cb);outline:none;}");
  out(".tr input[type=text]:focus{border-color:var(--bf);box-shadow:0 0 0 3px rgba(59,130,246,0.1);}");
  out(".sb{height:48px;padding:0 20px;font-size:1.05rem;font-weight:600;color:#fff;");
  out("background:var(--pc);border:none;border-radius:8px;cursor:pointer;white-space:nowrap;transition:background 0.2s;}");
  out(".sb:active{background:var(--ph);}");

  // -- Save button --
  out(".save-btn{width:100%;padding:12px;font-size:1.05rem;font-weight:700;font-family:inherit;");
  out("color:#fff;background:var(--sc);border:none;border-radius:var(--r);");
  out("cursor:pointer;margin-top:16px;box-shadow:var(--sh-sm);");
  out("transition:background .15s,box-shadow .15s;}");
  out(".save-btn:active{background:var(--sh);}");

  // -- Action button (e.g., Start) ---------------------------
  out(".action-btn{width:100%;padding:14px;font-size:1.05rem;font-weight:700;font-family:inherit;");
  out("color:#fff;background:linear-gradient(135deg,var(--sc),var(--sh));border:none;border-radius:var(--r);");
  out("cursor:pointer;margin-top:18px;box-shadow:var(--sh-md);");
  out("transition:background .15s,box-shadow .15s;}");
  out(".action-btn:active{background:var(--sh);}");

  // -- Hover (pointer devices only) --
  out("@media(hover:hover){");
  out(".page-btn:hover{background:var(--ph);box-shadow:var(--sh-md);}");
  out(".back-btn:hover{border-color:var(--ts);color:var(--tp);}");
  out(".save-btn:hover{background:var(--sh);box-shadow:var(--sh-md);}");
  out(".action-btn:hover{box-shadow:var(--sh-lg);}");
  out(".sb:hover{background:var(--ph);}");
  out("}");

  // -- Focus visible (keyboard nav) --
  out(":focus-visible{outline:2px solid var(--bf);outline-offset:2px;}");

  // -- Success overlay --
  out(".saved-overlay{position:fixed;top:0;left:0;right:0;bottom:0;");
  out("display:flex;align-items:center;justify-content:center;");
  out("background:rgba(0,0,0,.4);z-index:1000;opacity:1;transition:opacity .5s;}");
  out(".saved-inner{background:#fff;color:var(--sc);padding:32px 48px;");
  out("border-radius:var(--r);font-size:1.4rem;font-weight:700;");
  out("box-shadow:var(--sh-lg);text-align:center;}");

  // -- Responsive --
  out("@media(min-width:480px){#inputs{padding:28px 32px;}}");
  out("@media(min-width:640px){body{padding:24px;}#inputs{padding:32px 40px;}}");

  out("</style></head><body>");
  out("<div id=\"container\">");
  out("<div id=\"header\">"); out(displayLine1);
  if (displayLine2.length() > 0) {
    out("<div class=\"hl2\">"); out(displayLine2); out("</div>");
  }
  out("</div>");
  out("<div id=\"inputs\">");

  // Generate fields for the requested page
  for (int i = 0; i < _fieldCount; i++) {
    WPField& f = _fields[i];
    if (f.page != page) continue;
    if (f.condition != nullptr && !f.condition()) continue;

    switch (f.type) {
      case WP_DROPDOWN:
      case WP_DROPDOWN_OFFSET:  genDropDown(i);      break;
      case WP_RANGE:            genRange(i);          break;
      case WP_SUBHEADING:       genSubheading(i);     break;
      case WP_SEPARATOR:        genSeparator();       break;
      case WP_COLORPICKER:      genColorPicker(i);    break;
      case WP_TEXT:             genText(i);           break;
      case WP_PASSWORD:         genPassword(i);       break;
      case WP_CHECKBOX:         genCheckbox(i);       break;
      case WP_RADIO:            genRadio(i);          break;
      case WP_TIME:             genTime(i);           break;
      case WP_NUMBER:           genNumber(i);         break;
      case WP_DROPDOWN_RANGE:   genDropDownRange(i);  break;
      case WP_HIDDEN:           genHidden(i);         break;
      case WP_PAGE_BUTTON:      genPageButton(i);     break;
      case WP_ACTION_BUTTON:    genActionButton(i);   break;
      case WP_TEXT_INPUT:       genTextInput(i);      break;
      case WP_HTML:             genHTML(i);           break;
      case WP_BUTTON:           genButton(i);         break;
    }
  }

  // Buttons
  if (page == -1) {
    if (_mainHasFields) {
      out("<div class=\"sep\"></div>");
      out("<button type=\"button\" class=\"save-btn\" onclick=\"save()\">Save Settings</button>");
    }
  } else {
    out("<div class=\"sep\"></div>");
    out("<a href=\"/\" class=\"back-btn\">Back</a>");
    out("<button type=\"button\" class=\"save-btn\" onclick=\"save()\">Save Settings</button>");
  }

  out("</div></div>");
  out("<script>");
  // Helper: display a message overlay (same style as Settings Saved)
  // Tooltip toggle — close any other tooltip, then toggle this one with
  // smart above/below positioning based on available space.
  out("function tipToggle(id,btn){");
  out("var c=document.querySelectorAll('.tt.open');");
  out("for(var i=0;i<c.length;i++){if(c[i].id!=='tt_'+id){c[i].classList.remove('open');c[i].style.cssText='';}}");
  out("var t=document.getElementById('tt_'+id);if(!t)return;");
  out("if(t.classList.contains('open')){t.classList.remove('open');t.style.cssText='';return;}");
  // Portal: move tooltip to <body> so no ancestor can clip it. Position
  // fixed in viewport coords; this side-steps every overflow/positioning
  // edge case in form / sub-page / container CSS.
  out("if(t.parentNode!==document.body)document.body.appendChild(t);");
  out("var r=btn.getBoundingClientRect();");
  out("var vh=window.innerHeight,vw=window.innerWidth,m=16;");
  out("var below=vh-r.bottom-m,above=r.top-m;");
  out("var useBelow=below>=above;");
  out("var avail=useBelow?below:above;");
  out("var h=Math.max(140,Math.min(Math.floor(vh*0.7),avail));");
  // Position fixed; pin horizontal width to roughly the form's max width
  // centered horizontally. Vertical: anchor below icon if room, else above.
  out("var w=Math.min(vw-32,560);");
  // Center horizontally over the viewport (same axis as the form container).
  out("var leftPx=Math.max(16,Math.floor((vw-w)/2));");
  out("t.style.position='fixed';");
  out("t.style.left=leftPx+'px';");
  out("t.style.right='auto';");
  out("t.style.width=w+'px';");
  out("t.style.maxHeight=h+'px';");
  out("if(useBelow){t.style.top=(r.bottom+8)+'px';t.style.bottom='auto';}");
  out("else{t.style.top='auto';t.style.bottom=(vh-r.top+8)+'px';}");
  out("t.classList.add('open');}");
  // Edge/Chromium quirk: wheel events over an absolutely-positioned
  // overflow:auto element sometimes route to the page underneath instead
  // of the hovering scrollable child, leaving the tooltip unscrollable
  // even though the content overflows correctly. iOS Safari has no such
  // problem. Intercept wheel events inside any .tt-i and apply the delta
  // manually, then preventDefault to stop the page from scrolling.
  out("document.addEventListener('wheel',function(e){");
  out("var t=e.target.closest('.tt-i');if(!t)return;");
  out("var m=t.scrollHeight-t.clientHeight;if(m<=0)return;");
  out("var n=t.scrollTop+e.deltaY;if(n<0)n=0;if(n>m)n=m;");
  out("if(n!==t.scrollTop){t.scrollTop=n;e.preventDefault();}");
  out("},{passive:false});");
  // Close any open tooltip when the user touches the form anywhere outside
  // an info icon. Tapping the tooltip box itself also closes it.
  out("document.addEventListener('click',function(e){");
  out("if(e.target.closest('.info'))return;");
  out("var c=document.querySelectorAll('.tt.open');");
  out("for(var i=0;i<c.length;i++)c[i].classList.remove('open');});");
  out("function showMsg(t){var e=document.querySelector('.saved-overlay');if(e)e.remove();");
  out("var o=document.createElement('div');o.className='saved-overlay';");
  out("o.innerHTML='<div class=\"saved-inner\">'+t+'</div>';");
  out("document.body.appendChild(o);");
  out("setTimeout(function(){o.style.opacity='0';setTimeout(function(){o.remove();},500);},3000);}");
  // Helper: process AJAX response — show overlay if body is not "OK"
  out("function chk(r){return r.text().then(function(m){"
        "if(m&&m.indexOf('::RL::')===0){var msg=m.substr(6);if(msg)showMsg(msg);"
        "try{sessionStorage.setItem('wpsy',window.scrollY);}catch(e){}"
        "setTimeout(function(){location.replace(location.href);},msg?900:120);return;}"
        "if(m&&m!=='OK')showMsg(m);"
      "});}");
  // Action button "confirm and clear" — fire-and-forget AJAX then replace
  // entire body with the confirmation overlay.
  // data-reload set: poll the server (1.5 s cadence, 4 s per-try timeout)
  //   starting 3 s out, then go to the HOME page as soon as it answers. Goes
  //   home (not location.reload of the current sub-page) because device-level
  //   status lives on the home page. Covers the no-reboot case (server returns
  //   quickly) and the reboot case (server comes back on new firmware).
  //   data-statusfield also set: poll '/?field=<sf>' whose text body is the
  //   action RESULT. The blocking action doesn't answer until it finishes, so
  //   the first non-error reply is the real outcome; show it in the overlay
  //   for ~5 s (so it doesn't depend on a home-page status box / cache / timing
  //   window), then go home. "" or "OK" means no result → go home at once.
  // data-reload unset: overlay fades out after 2 s (reboot/AP actions whose
  //   message should linger; the server isn't coming back at the same URL).
  out("function actionClear(f,btn){fetch('/?field='+f+'&value=1');");
  out("document.body.innerHTML='<div class=\"saved-overlay\"><div class=\"saved-inner\">'+btn.dataset.msg+'</div></div>';");
  out("if(btn.dataset.reload){var sf=btn.dataset.statusfield;");
  // Go HOME, but wait until the device is actually serving again before
  // navigating — a successful OTA reboots, so a blind redirect would land
  // mid-reboot, fail, and strand the user on the sub-page. Ping '/' until it
  // answers, then replace to home. No-reboot actions (already current) answer
  // immediately, so this is effectively instant for them.
  out("var home=function(){var c=new AbortController();var t=setTimeout(function(){c.abort();},3000);");
  out("fetch('/?cb='+Date.now(),{cache:'no-store',signal:c.signal}).then(function(){clearTimeout(t);location.replace('/');}).catch(function(){clearTimeout(t);setTimeout(home,1500);});};");
  out("var show=function(m){var e=document.querySelector('.saved-inner');if(e&&m)e.innerHTML=m;setTimeout(home,m?5000:0);};");
  out("var poll=function(){var c=new AbortController();");
  out("var t=setTimeout(function(){c.abort();},4000);");
  out("var u=sf?('/?field='+sf+'&value='+Date.now()):'/?ping=1';");
  out("fetch(u,{cache:'no-store',signal:c.signal}).then(function(r){clearTimeout(t);");
  out("if(sf){r.text().then(function(m){show(m&&m!=='OK'?m:'');});}else{home();}})");
  out(".catch(function(){clearTimeout(t);setTimeout(poll,1500);});};setTimeout(poll,3000);return;}");
  out("setTimeout(function(){var o=document.querySelector('.saved-overlay');");
  out("if(o){o.style.opacity='0';setTimeout(function(){if(o)o.remove();},500);}},2000);}");
  // Slider thumb/badge tinting: data-tc="r|g|b" = dynamic channel, else literal CSS color
  out("function stc(el){var tc=el.getAttribute('data-tc');if(!tc)return;var v=el.value;");
  out("var c=tc=='r'?'rgb('+v+',0,0)':tc=='g'?'rgb(0,'+v+',0)':tc=='b'?'rgb(0,0,'+v+')':tc;");
  out("el.style.setProperty('--tc',c);var b=document.getElementById(el.id+'_v');");
  out("if(b)b.style.setProperty('--tc',c);}");
  out("document.querySelectorAll('[data-tc]').forEach(function(el){stc(el);});");
  // Field change senders — all use chk() so showMessage() works universally.
  // _safeFetch wraps the fetch with a 3 s AbortController timeout and a silent
  // catch. Without these, a single hung request (server busy / WiFi drop) can
  // tie up Safari's per-origin connection slots (~6) and make subsequent
  // slider / dropdown changes appear to "stop working" until the browser
  // gives up ~30 s later. Failed/aborted requests are swallowed silently.
  out("function _safeFetch(url){var c=new AbortController();");
  out("var t=setTimeout(function(){c.abort();},3000);");
  out("return fetch(url,{signal:c.signal}).then(function(r){clearTimeout(t);return chk(r);})");
  out(".catch(function(){clearTimeout(t);});}");
  // Per-field debouncer. Range inputs fire oninput continuously during a
  // drag; coalesce to one send per ~150 ms of inactivity per field. clr()
  // is the same idea for color pickers — parses '#RRGGBB' to int before
  // send. Both share _rngT (keyed by field id, no collision).
  out("var _rngT={};function rng(el){var f=el.id;clearTimeout(_rngT[f]);");
  out("_rngT[f]=setTimeout(function(){send(f,el.value);},150);}");
  out("function clr(el){var f=el.id;clearTimeout(_rngT[f]);");
  out("_rngT[f]=setTimeout(function(){send(f,parseInt(el.value.substring(1),16));},150);}");
  // Field gating (declared via gateFieldBy()): each entry below makes one
  // input disabled unless its controller field has the configured value.
  // No-op when no pairs are registered.
  if (_gatePairCount > 0) {
    out("[");
    for (int i = 0; i < _gatePairCount; ++i) {
      if (i > 0) out(",");
      out("{g:'");      out(_gatePairs[i].gatedField);
      out("',c:'");     out(_gatePairs[i].controllerField);
      out("',v:");      out(_gatePairs[i].enableValue);
      out("}");
    }
    out("].forEach(function(p){");
    out("var g=document.getElementById(p.g),c=document.getElementById(p.c);");
    out("if(!g||!c)return;");
    out("var fg=g.closest('.fg');");
    out("function u(){var on=(parseInt(c.value,10)===p.v);");
    out("if(fg)fg.style.display=on?'':'none';g.disabled=!on;}");
    out("c.addEventListener('change',u);u();");
    out("});");
  }
  out("function send(f,v){_safeFetch('/?field='+f+'&value='+v);}");
  out("function sendStr(f,v){_safeFetch('/?field='+f+'&value='+encodeURIComponent(v));}");
  out("function sendText(id){var m=document.getElementById(id);if(m&&m.value.trim()!=''){_safeFetch('/?field='+id+'&value='+encodeURIComponent(m.value.trim()));m.focus();}}");
  if (_rebootOnSave) {
    out("function save(){fetch('/?save=1');");
    out("document.body.innerHTML='<div class=\"saved-overlay\"><div class=\"saved-inner\">\\u2713 Settings Saved</div></div>';}");
  } else {
    // Save: if response is "OK", show "Settings Saved"; else show the custom message
    out("function save(){fetch('/?save=1').then(function(r){return r.text();}).then(function(m){");
    out("var t=(m&&m!=='OK')?m:'\\u2713 Settings Saved';showMsg(t);});}");
  }
  // After a chk()-triggered reload (rule add/edit/delete) restore the scroll
  // position so the form doesn't jump to the top, and scrollTo(0,...) re-pins
  // horizontal to 0 so the left-right lock is honored. One-shot (key removed).
  out("try{var _y=sessionStorage.getItem('wpsy');if(_y!==null){sessionStorage.removeItem('wpsy');window.scrollTo(0,parseInt(_y)||0);}}catch(e){}");
  out("</script></body></html>");

  // Send headers + body. Use a small char buffer for the headers to avoid String.
  // Cache-Control: no-store prevents browsers from caching the form HTML, so
  // that changes to page names / field bindings / etc. take effect on the
  // next page load instead of waiting for a hard refresh.
  char headers[160];
  snprintf(headers, sizeof(headers),
    "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: %d\r\n"
    "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
    _htmlPos);
  client.print(headers);
  writeAll(client, (const uint8_t*)_htmlBuf, _htmlPos);
  client.flush();
  client.stop();
}

// Stream a buffer to the client in segment-sized chunks with a progress-based
// idle timeout. A single client.write() of a large body is bounded by the
// socket's setTimeout(50) (set for fast request reads), which is far too short
// to flush a 20-40 KB form over a slow AP link: write() returns short and the
// response is truncated — the browser renders nothing, while small responses
// (e.g. /health, one segment) survive. Chunked send keeps pushing as the TCP
// window drains and only gives up after a real stall, so it can't hang on a
// dead client.
void WebPanel::writeAll(WiFiClient& client, const uint8_t* buf, int len) {
  const int CHUNK = 1460;            // ~one TCP segment
  int sent = 0;
  unsigned long start = millis();
  unsigned long lastProgress = start;
  // 2 s NO-PROGRESS ceiling. The timer resets on every successful write, so a
  // healthy or merely-slow (but advancing) client never trips it — it only
  // bounds how long a genuinely dead/stalled socket can block the caller's
  // loop(). Kept short so that callers whose loop also services time-critical
  // work (single-loop clocks under a 30 s software watchdog, NtpServer's UDP
  // request servicing) aren't stalled long, even if handleClient() drains
  // several stalled sockets back-to-back in one pass.
  while (sent < len && client.connected() && millis() - lastProgress < 2000) {
    int n = len - sent;
    if (n > CHUNK) n = CHUNK;
    size_t w = client.write(buf + sent, n);
    if (w > 0) {
      sent += (int)w;
      lastProgress = millis();       // reset idle timer on any forward progress
    } else {
      delay(2);                      // window full — let it drain, then retry
    }
  }
}
