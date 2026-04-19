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
void WebPanel::setRebootOnSave(bool reboot) { _rebootOnSave = reboot; }

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
    _htmlBuf = (char*)ps_malloc(WP_HTML_BUFFER_SIZE);
  }
#endif
  if (_htmlBuf == nullptr) {
    _htmlBuf = (char*)malloc(WP_HTML_BUFFER_SIZE);
  }
  if (_htmlBuf) {
    _htmlBufSize = WP_HTML_BUFFER_SIZE;
    _htmlBuf[0] = 0;
  }
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

void WebPanel::addPage(const String& line1, const String& line2) {
  if (_numPages >= WP_MAX_PAGES) return;
  ensureFields();

  // Nav button label: prefer line2 (the page-specific name) if non-empty,
  // otherwise fall back to line1.
  String buttonLabel = (line2.length() > 0) ? line2 : line1;

  // Add a page button field on the main page
  if (_fieldCount < _maxFields) {
    WPField& f = _fields[_fieldCount++];
    f.type = WP_PAGE_BUTTON;
    f.label = buttonLabel;
    f.fieldName = "";
    f.presetPtr = nullptr;
    f.strPtr = nullptr;
    f.condition = nullptr;
    f.page = -1;  // button lives on main page
    f.offset = _numPages;  // store page index
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
                                const String& confirmMessage) {
  ensureFields();
  if (_fieldCount >= _maxFields) return;
  WPField& f = _fields[_fieldCount++];
  f.type = WP_ACTION_BUTTON;
  f.label = label;
  f.fieldName = fieldName;
  f.extraText = confirmMessage;   // reused: confirm message text
  f.presetPtr = nullptr;
  f.strPtr = nullptr;
  f.condition = nullptr;
  f.page = -1;   // always on home page regardless of _currentPage
  // Do NOT set _mainHasFields — action button does not trigger the
  // home-page Save button.
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
                             const String& buttonLabel, const char* tip) {
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
  f.tip = tip;
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

    // Wait briefly for HTTP request data to arrive after TCP handshake.
    unsigned long waitStart = millis();
    while (!client.available() && client.connected() && millis() - waitStart < 200) {
      delay(1);
    }
    if (!client.available()) { client.stop(); continue; }

    client.setTimeout(50);
    String req = client.readStringUntil('\r');

    // Read just enough headers for auth check, then discard the rest
    String headers = "";
    if (_authPass && _authPass->length() > 0) {
      headers = client.readString();
      if (!checkAuth(headers)) {
        send401(client);
        continue;
      }
    } else {
      // Drain remaining data without storing it
      while (client.available()) client.read();
    }

    // Reject favicon and other non-form requests quickly
    if (req.indexOf("GET / ") < 0 && req.indexOf("GET /?") < 0 && req.indexOf("GET /page") < 0) {
      client.print("HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n");
      client.flush();
      client.stop();
      continue;
    }

    if (req.indexOf("/?save=1") >= 0) {
      handleSave(client);
      continue;
    }

    if (req.indexOf("/?field=") >= 0) {
      handleAjax(client, req);
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
  }
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
  out("<select onchange=\"send('");
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
  out("\" oninput=\"document.getElementById('"); out(f.fieldName);
  out("_v').textContent=this.value;stc(this)\" onchange=\"send('");
  out(f.fieldName); out("',this.value)\">");
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
  out("<input type=\"color\" value=\"");
  out(hex);
  out("\" onchange=\"send('");
  out(f.fieldName);
  out("',parseInt(this.value.substring(1),16))\"></div>");
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

  out("<div class=\"fg\"><label class=\"fl\">");
  out(f.label);
  emitTipIcon(idx);
  out("</label>");
  emitTipBox(idx);
  out("<div class=\"tr\"><input type=\"text\" id=\"");
  out(f.fieldName);
  out("\" value=\"");
  out(val);
  out("\"");
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
  out(" onkeydown=\"if(event.key==='Enter')sendText('");
  out(f.fieldName);
  out("')\">");
  out("<button class=\"sb\" onclick=\"sendText('");
  out(f.fieldName);
  out("')\">");
  out(f.extraText);
  out("</button></div></div>");
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
  out("<select onchange=\"send('");
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
  out("\" class=\"page-btn\">");
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
    out("\" onclick=\"actionClear('");
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
// Uses event.stopPropagation so the click doesn't bubble to parent label.
void WebPanel::emitTipIcon(int idx) {
  WPField& f = _fields[idx];
  if (!f.tip || f.tip[0] == '\0') return;
  out(" <span class=\"info\" onclick=\"event.stopPropagation();tipToggle('");
  out(f.fieldName);
  out("',this)\">&#9432;</span>");
}

// Emit the floating tooltip bubble div. No-op if tip empty.
void WebPanel::emitTipBox(int idx) {
  WPField& f = _fields[idx];
  if (!f.tip || f.tip[0] == '\0') return;
  out("<div class=\"tt\" id=\"tt_");
  out(f.fieldName);
  out("\">");
  out(f.tip);
  out("</div>");
}

// -- Form rendering ------------------------------------------------------

void WebPanel::serveForm(WiFiClient& client, int page) {
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
  // Browser title bar shows line2 if present, else line1
  String docTitle = (displayLine2.length() > 0) ? displayLine2 : displayLine1;

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
  out("--th:"); out(_sliderTrack); out("px;--ts:"); out(_sliderThumb); out("px;");
  out("--sh-sm:0 1px 3px rgba(0,0,0,.08);");
  out("--sh-md:0 4px 12px rgba(0,0,0,.1);");
  out("--sh-lg:0 12px 28px rgba(0,0,0,.12);");
  out("}");

  // -- Reset & base --
  out("*{box-sizing:border-box;margin:0;padding:0;}");
  out("body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,'Helvetica Neue',sans-serif;");
  out("background:var(--bg);color:var(--tp);line-height:1.6;padding:16px;");
  out("-webkit-font-smoothing:antialiased;-moz-osx-font-smoothing:grayscale;}");

  // -- Container --
  out("#container{max-width:640px;margin:0 auto;background:var(--cb);");
  out("border-radius:var(--r);box-shadow:var(--sh-lg);overflow:hidden;}");

  // -- Header --
  out("#header{background:linear-gradient(135deg,var(--pc),var(--pd));");
  out("color:#fff;text-align:center;font-size:clamp(1.1rem,5vw,1.7rem);font-weight:700;");
  out("padding:8px 24px;letter-spacing:-.3px;line-height:1.1;white-space:nowrap;}");
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
  // Dark slate bubble — always in DOM, fades in via opacity transition
  out(".tt{position:absolute;left:0;right:0;z-index:100;");
  out("background:var(--tp);color:#fff;border:none;border-radius:12px;");
  out("padding:10px 14px;font-size:.9rem;font-weight:400;");
  out("text-transform:none;letter-spacing:0;line-height:1.4;");
  out("box-shadow:0 6px 16px rgba(0,0,0,.25);cursor:pointer;top:1.8rem;");
  out("opacity:0;visibility:hidden;");
  out("transition:opacity .15s ease,visibility .15s;pointer-events:none;}");
  out(".tt.open{opacity:1;visibility:visible;pointer-events:auto;}");
  out(".tt.above{top:auto;bottom:100%;margin-bottom:8px;}");
  // Arrow — single triangle in the dark bubble color
  out(".tt::before{content:'';position:absolute;top:-8px;left:1.5rem;");
  out("border:8px solid transparent;border-top:0;border-bottom-color:var(--tp);}");
  out(".tt.above::before{top:auto;bottom:-8px;");
  out("border-top:8px solid var(--tp);border-bottom:0;}");

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
  out("width:var(--ts);height:var(--ts);background:var(--tc,var(--pc));border-radius:50%;cursor:pointer;");
  out("box-shadow:0 1px 4px rgba(0,0,0,.2);transition:transform .15s,background .1s;}");
  out("input[type=range]::-moz-range-thumb{width:var(--ts);height:var(--ts);background:var(--tc,var(--pc));");
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
  out(".sb{height:48px;padding:0 18px;font-size:.95rem;font-weight:600;color:#fff;font-family:inherit;");
  out("background:var(--pc);border:none;border-radius:var(--ri);cursor:pointer;");
  out("white-space:nowrap;transition:background .15s;}");
  out(".sb:active{background:var(--pd);}");

  // -- Password --
  out(".pw{display:flex;align-items:center;gap:10px;}");
  out(".pw input[type=password],.pw input[type=text]{flex:1;}");
  out(".sp{display:flex;align-items:center;cursor:pointer;font-size:.8rem;");
  out("color:var(--ts);white-space:nowrap;user-select:none;}");
  out(".sp input[type=checkbox]{width:16px;height:16px;margin-right:5px;cursor:pointer;}");

  // -- Checkbox --
  out(".cb-group{margin-bottom:8px;}");
  out(".cb-label{display:flex;align-items:center;cursor:pointer;");
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
  out(".back-btn{display:block;width:100%;padding:12px;font-size:1rem;font-weight:600;");
  out("color:var(--ts);background:var(--bg);border:1.5px solid var(--br);");
  out("border-radius:var(--r);cursor:pointer;text-align:center;text-decoration:none;");
  out("transition:border-color .15s,color .15s;margin-bottom:10px;}");
  out(".back-btn:active{border-color:var(--ts);color:var(--tp);}");

  // -- Text input with send button --
  out(".tr{display:flex;gap:10px;align-items:center;}");
  out(".tr input[type=text]{flex:1;height:48px;padding:12px;font-size:1.1rem;");
  out("border:2px solid var(--br);border-radius:8px;background:var(--cb);outline:none;}");
  out(".tr input[type=text]:focus{border-color:var(--bf);box-shadow:0 0 0 3px rgba(59,130,246,0.1);}");
  out(".sb{height:48px;padding:0 20px;font-size:1.1rem;font-weight:600;color:#fff;");
  out("background:var(--pc);border:none;border-radius:8px;cursor:pointer;white-space:nowrap;transition:background 0.2s;}");
  out(".sb:active{background:var(--ph);}");

  // -- Save button --
  out(".save-btn{width:100%;padding:12px;font-size:1.05rem;font-weight:700;font-family:inherit;");
  out("color:#fff;background:var(--sc);border:none;border-radius:var(--r);");
  out("cursor:pointer;margin-top:16px;box-shadow:var(--sh-sm);");
  out("transition:background .15s,box-shadow .15s;}");
  out(".save-btn:active{background:var(--sh);}");

  // -- Action button (e.g., Start) ---------------------------
  out(".action-btn{width:100%;padding:14px;font-size:1.1rem;font-weight:700;font-family:inherit;");
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
  out("for(var i=0;i<c.length;i++){if(c[i].id!=='tt_'+id)c[i].classList.remove('open');}");
  out("var t=document.getElementById('tt_'+id);if(!t)return;");
  out("if(t.classList.contains('open')){t.classList.remove('open');return;}");
  out("var r=btn.getBoundingClientRect();");
  out("var below=window.innerHeight-r.bottom,above=r.top;");
  out("if(below<150&&above>below)t.classList.add('above');");
  out("else t.classList.remove('above');");
  out("t.classList.add('open');}");
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
  out("function chk(r){return r.text().then(function(m){if(m&&m!=='OK')showMsg(m);});}");
  // Action button "confirm and clear" — fire-and-forget AJAX then replace
  // entire body with the confirmation overlay. The overlay fades out after
  // 2 seconds (same timing as the Settings Saved overlay).
  out("function actionClear(f,btn){fetch('/?field='+f+'&value=1');");
  out("document.body.innerHTML='<div class=\"saved-overlay\"><div class=\"saved-inner\">'+btn.dataset.msg+'</div></div>';");
  out("setTimeout(function(){var o=document.querySelector('.saved-overlay');");
  out("if(o){o.style.opacity='0';setTimeout(function(){if(o)o.remove();},500);}},2000);}");
  // Slider thumb/badge tinting: data-tc="r|g|b" = dynamic channel, else literal CSS color
  out("function stc(el){var tc=el.getAttribute('data-tc');if(!tc)return;var v=el.value;");
  out("var c=tc=='r'?'rgb('+v+',0,0)':tc=='g'?'rgb(0,'+v+',0)':tc=='b'?'rgb(0,0,'+v+')':tc;");
  out("el.style.setProperty('--tc',c);var b=document.getElementById(el.id+'_v');");
  out("if(b)b.style.setProperty('--tc',c);}");
  out("document.querySelectorAll('[data-tc]').forEach(function(el){stc(el);});");
  // Field change senders — all use chk() so showMessage() works universally
  out("function send(f,v){fetch('/?field='+f+'&value='+v).then(chk)}");
  out("function sendStr(f,v){fetch('/?field='+f+'&value='+encodeURIComponent(v)).then(chk)}");
  out("function sendText(id){var m=document.getElementById(id);if(m&&m.value.trim()!=''){fetch('/?field='+id+'&value='+encodeURIComponent(m.value.trim())).then(chk);m.focus();}}");
  if (_rebootOnSave) {
    out("function save(){fetch('/?save=1');");
    out("document.body.innerHTML='<div class=\"saved-overlay\"><div class=\"saved-inner\">\\u2713 Settings Saved</div></div>';}");
  } else {
    // Save: if response is "OK", show "Settings Saved"; else show the custom message
    out("function save(){fetch('/?save=1').then(function(r){return r.text();}).then(function(m){");
    out("var t=(m&&m!=='OK')?m:'\\u2713 Settings Saved';showMsg(t);});}");
  }
  out("</script></body></html>");

  // Send headers + body. Use a small char buffer for the headers to avoid String.
  char headers[96];
  snprintf(headers, sizeof(headers),
    "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: %d\r\nConnection: close\r\n\r\n",
    _htmlPos);
  client.print(headers);
  client.write((const uint8_t*)_htmlBuf, _htmlPos);
  client.flush();
  client.stop();
}
