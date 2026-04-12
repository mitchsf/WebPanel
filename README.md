# WebPanel

A self-contained, dependency-free settings UI library for ESP32. Build a polished mobile-friendly web form bound to your variables in ~10 lines of setup code, and let users edit values from any browser on the network.

WebPanel renders HTML/CSS/JavaScript directly from C++ — no SPIFFS upload, no template files, no external HTTP framework. The entire UI is generated into a single static heap buffer so memory use is predictable and there is zero heap fragmentation at runtime.

```cpp
#include "WebPanel.h"

WiFiServer server(80);
WebPanel panel;

int   brightness = 5;
int   mode       = 1;
String ssid      = "MyNetwork";

void setup() {
  WiFi.softAP("MyDevice");
  WebPanel::allocBuffer();          // call once, early in setup()
  panel.setTitle("My Device", "Settings");
  panel.setSaveCallback([]() { /* persist to NVS */ });
  panel.begin(&server);
  server.begin();

  panel.addRange("Brightness", "br", 0, 9, &brightness);
  panel.addDropDown("Mode", "mode", "Off, Slow, Fast", &mode);
  panel.addText("WiFi SSID", "ssid", &ssid);
}

void loop() { panel.handleClient(); }
```

That's the entire app. Connect to the device's WiFi, open `http://192.168.4.1`, and you have a working settings page.

---

## Table of contents

- [What it is](#what-it-is)
- [Features](#features)
- [Installation](#installation)
- [Hello World](#hello-world)
- [How it works](#how-it-works)
- [Memory model](#memory-model)
- [API reference](#api-reference)
  - [Setup](#setup)
  - [Authentication](#authentication)
  - [Field types](#field-types)
  - [Pages and navigation](#pages-and-navigation)
  - [Action buttons](#action-buttons)
  - [Tooltips](#tooltips)
  - [Callbacks](#callbacks)
  - [Sending messages from callbacks](#sending-messages-from-callbacks)
  - [Conditional fields](#conditional-fields)
- [Configuration constants](#configuration-constants)
- [Live updates vs Save](#live-updates-vs-save)
- [Limitations](#limitations)
- [License](#license)

---

## What it is

WebPanel is for projects where you need to expose a handful (or several dozen) tunable parameters to a non-technical user without writing any HTML, CSS, or JavaScript yourself. Typical applications:

- IoT device configuration (initial WiFi setup, cloud credentials, NTP servers)
- Long-term runtime tuning (brightness, schedules, sensor offsets, color presets)
- Diagnostic and calibration screens
- One-shot action triggers (factory reset, OTA, reboot) with confirmation overlays

It is **not** a general-purpose web framework. It serves exactly one form (which may have multiple pages), one Save endpoint, and one AJAX endpoint per device. There are no routes, middleware, or session handling — and that simplicity is the point.

## Features

- **Zero dependencies.** Only `WiFi.h`/`WiFiServer.h`/`WiFiClient.h` from the ESP32 core. No `WebServer`, `ESPAsyncWebServer`, AsyncTCP, ArduinoJson, or filesystem libraries needed.
- **Bound directly to your variables.** Pass `int*` (or `String*`) pointers — when the user changes a slider, your variable updates immediately. No JSON parsing, no manual extraction.
- **Live AJAX updates.** Every field change is sent to the device the moment the user releases it. No "submit" required for runtime tuning. A separate "Save Settings" button calls your save callback to persist values.
- **Multi-page forms.** A home page with navigation buttons, plus up to 10 sub-pages. Each sub-page has a Back button and its own Save button. Helps split long forms into logical sections (Network, Display, Lighting, etc.) and keeps each rendered page small.
- **Polished mobile UI.** Modern CSS (gradients, shadows, transitions, focus rings), 48 px touch targets, responsive layout, supports both light DPI phones and desktop browsers. iOS Safari time-input fix included.
- **Tooltips.** Pass an optional `tip` string to any field; an info icon (ⓘ) appears next to the label and a floating dark bubble pops up on tap.
- **Action buttons.** Trigger arbitrary one-shot operations (reboot, OTA, factory reset) with optional full-screen confirmation overlays.
- **`showMessage()` from any callback.** Display a custom toast over the form when an action completes or validation fails.
- **Optional HTTP Basic Auth.** Protect a form with a password via `setAuth()`. The username field is ignored — only the password is checked. Enforced on all requests when the password string is non-empty; disabled when empty.
- **16 field types.** Dropdowns, ranges, color pickers, time inputs, text/password, text input with send button, checkboxes, radio groups, numeric, hidden, page nav, action buttons, subheadings, separators.
- **Static render buffer.** The entire HTML page (~12–35 KB depending on field count) is built into a single pre-allocated buffer. After `setup()`, no further heap allocation occurs and free memory stays flat.

## Installation

1. Copy `WebPanel.h` and `WebPanel.cpp` into either your sketch folder or your Arduino libraries folder.
2. `#include "WebPanel.h"` in your sketch.
3. Make sure your project links against the ESP32 core (Tools → Board → ESP32-family board).

There is no `library.properties` yet — add one if you publish to the Library Manager.

## Hello World

A complete sketch that creates an Access Point, serves a one-page settings form, and prints field changes to Serial:

```cpp
#include <WiFi.h>
#include "WebPanel.h"

WiFiServer server(80);
WebPanel panel;

int   brightness = 5;
int   mode       = 1;
int   color      = 0xFF8000;
String name      = "Atrium Lamp";

void onChange(const String& field, int value) {
  Serial.print(field); Serial.print(" = "); Serial.println(value);
}

void onText(const String& field, const String& value) {
  Serial.print(field); Serial.print(" = "); Serial.println(value);
}

void onSave() {
  Serial.println("Save pressed — write to NVS here");
}

void setup() {
  Serial.begin(115200);

  WebPanel::allocBuffer();          // CALL ONCE, EARLY — see Memory model
  WiFi.softAP("WebPanel-Demo");

  panel.setTitle("Demo Device", "Settings");
  panel.setSaveCallback(onSave);
  panel.setOnChange(onChange);
  panel.setOnTextChange(onText);
  panel.begin(&server);
  server.begin();

  panel.addText      ("Device Name",   "name",  &name);
  panel.addRange     ("Brightness",    "br",    0, 9, &brightness);
  panel.addDropDown  ("Mode",          "mode",  "Off, Slow, Medium, Fast", &mode);
  panel.addColorPicker("Accent Color", "color", &color);
}

void loop() {
  panel.handleClient();
}
```

Connect a phone to the `WebPanel-Demo` WiFi network, open `http://192.168.4.1`, and you'll see a polished settings form with live Serial output as you drag the slider, pick options, or type a name.

## How it works

WebPanel follows a strict request/response model on top of the ESP32 core's `WiFiServer`:

1. **Page render (`GET /` or `GET /pageN`).** WebPanel writes a complete HTML document — `<style>` block, all field markup, and a small `<script>` that owns the AJAX helpers — into a static char buffer, then writes that buffer over the socket in one go. No template engine, no SPIFFS reads.
2. **Field change (`GET /?field=NAME&value=N`).** The browser fires a `fetch()` the moment the user releases a slider, picks a dropdown, types in a text input, etc. WebPanel's `handleAjax()` finds the matching field by name and writes the new value into the bound `int*` or `String*`. If you registered a change callback, it fires immediately.
3. **Save (`GET /?save=1`).** Triggered by the green "Save Settings" button on the home or any sub-page. Calls your `setSaveCallback()` function. The browser shows an animated "✓ Settings Saved" overlay. Optionally the device can reboot after save (`setRebootOnSave(true)`).
4. **Action button (`GET /?field=NAME&value=1`).** Same wire format as a field change, but for buttons that trigger one-shot operations. If a confirmation message was provided, the browser replaces the entire page with the confirmation overlay (used for actions that reboot the device, since the response will never arrive).

The whole HTTP layer is roughly 60 lines: parse request line, dispatch on URL prefix, serve. There is no `WebServer` object; WebPanel uses `WiFiServer::accept()` and `WiFiClient` directly so it has no extra dependencies and has full control over the response timing (which matters for fast slider drags — a brief 200 ms wait for the full HTTP request prevents dropped packets).

## Memory model

WebPanel uses a single static heap allocation for the HTML render buffer, sized by `WP_HTML_BUFFER_SIZE` (default **40 KB**). It is allocated once via `WebPanel::allocBuffer()` and reused for every page render — the buffer is reset (`_htmlPos = 0`) at the start of each `serveForm()` call.

```cpp
void setup() {
  WebPanel::allocBuffer();   // ← call as the FIRST heap-touching line
  WiFi.softAP(...);
  // ...rest of setup
}
```

**Why allocate this early?** The ESP32's heap fragments easily. Allocating the 40 KB buffer before any other dynamic allocation places it at a low, stable address; subsequent `String` allocations and WiFi stack allocations sit above it. The buffer never moves and never gets freed, so the largest free block stays large for the rest of the program's life.

If you forget `allocBuffer()`, `begin()` will fall back to allocating it at that point. But if WiFi has already started, the buffer may end up in a high address with fragmented free space below it, and your largest free block can drop by 30–40% within minutes.

**Sizing the buffer.** Each rendered page typically needs ~150–500 bytes per field (the static CSS block alone is ~5 KB, identical on every page). For most projects:

| Fields per page | Recommended `WP_HTML_BUFFER_SIZE` |
|---|---|
| Up to ~25 | 16 KB |
| Up to ~50 | 24 KB |
| Up to ~80 | 40 KB (default) |
| 100+ | 64 KB — also call `setMaxFields()` |

If you split a long form across multiple pages with `addPage()`, each page renders independently and you can reduce the buffer because the largest individual page is smaller than the total field count.

## API reference

### Setup

```cpp
WebPanel panel;
WiFiServer server(80);
```

```cpp
static void WebPanel::allocBuffer();
```
Allocate the shared HTML render buffer. **Call once, very early in `setup()`** — before WiFi.begin or any other heap activity. Idempotent: safe to call again, no-op if already allocated.

```cpp
void setMaxFields(int maxFields);
```
Set the maximum number of fields for this instance. Call before any `add*()` calls. If not called, defaults to 80. The field array is heap-allocated via `calloc()`, so you only use as much memory as you need. For a small form with 10 fields, call `panel.setMaxFields(20)` to save RAM compared to the default.

```cpp
void setTitle(const String& line1, const String& line2 = "");
```
Sets the home page header. Two-line variant: `line1` is the large header, `line2` is the smaller subtitle below it. The browser tab title uses `line2` if present, otherwise `line1`.

```cpp
void setSaveCallback(WPSaveCallback cb);     // typedef void (*WPSaveCallback)();
void setOnChange(WPChangeCallback cb);       // typedef void (*WPChangeCallback)(const String& field, int value);
void setOnTextChange(WPTextCallback cb);     // typedef void (*WPTextCallback)(const String& field, const String& value);
void setRebootOnSave(bool reboot);
void begin(WiFiServer* server);
void handleClient();   // call from loop()
```

### Authentication

```cpp
void setAuth(String* password);
```
Enable optional HTTP Basic Auth on this form instance. Pass a pointer to a `String` variable containing the password. Auth is enforced on every request (page loads and AJAX calls) whenever `*password` is non-empty. When the string is empty, all requests are allowed through without a prompt.

The browser's native Basic Auth dialog will show both a username and password field — this is standard browser behavior and cannot be suppressed. **WebPanel ignores the username entirely**; only the password is checked. Users can type anything (or nothing) in the username field.

Call `setAuth()` before `begin()`, during form setup:

```cpp
String livePassword = "mypass";

void setup() {
  WebPanel::allocBuffer();
  WiFi.softAP("MyDevice");

  panel.setTitle("My Device", "Settings");
  panel.setAuth(&livePassword);       // protect this form with a password
  panel.setSaveCallback(onSave);
  panel.begin(&server);
  server.begin();

  panel.addRange("Brightness", "br", 0, 9, &brightness);
}
```

Because the password is bound by pointer, you can change it at runtime (e.g. let the user set it from a separate setup form) and the new value takes effect immediately — no restart required. Setting it to an empty string disables auth entirely.

**Security notes:**
- HTTP Basic Auth sends credentials base64-encoded (not encrypted). On a WPA2 home network this is fine — traffic is already encrypted at the link layer. Do not rely on it over an open network.
- There is no rate limiting on failed attempts, but brute-forcing over WiFi against an ESP32 handling one connection at a time is impractically slow.
- The form controls device settings only (brightness, colors, modes). There is no file system access, shell, or firmware upload surface behind it.

### Field types

Every interactive field method takes an optional final `tip` parameter — if non-empty, an info icon (ⓘ) appears next to the label and tapping it shows a floating dark tooltip bubble.

#### Dropdowns

```cpp
void addDropDown(const String& label, const String& field,
                 const String& options, int* preset,
                 const String& tip = "");
```
Standard dropdown. `options` is a CSV string (e.g. `"Off, Low, Medium, High"`). Option indexes are zero-based: the bound `int` will be 0..N-1. Whitespace around commas is trimmed.

```cpp
void addDropDownOffset(const String& label, const String& field,
                       const String& options, int* preset, int offset,
                       const String& tip = "");
```
Same as `addDropDown` but the bound integer values are shifted by `offset`. Useful when your existing variable uses 1-based indexing (`preset[31] = 1..4`) — pass `offset = 1` and the dropdown will store/retrieve 1..N instead of 0..N-1.

```cpp
void addDropDownRange(const String& label, const String& field,
                      int minVal, int maxVal, int* preset,
                      const String& tip = "");
```
Auto-generates a dropdown of integer values from `minVal` to `maxVal` inclusive. No CSV needed. Good for "Hour" (0..23), "Day" (1..31), etc.

#### Range slider

```cpp
void addRange(const String& label, const String& field,
              int minVal, int maxVal, int* preset,
              const String& tip = "");
```
Native HTML5 range slider with the current value displayed in a colored badge to the right. Step size is always 1. The badge updates live as the user drags; the AJAX call fires when the user releases.

#### Numeric input

```cpp
void addNumber(const String& label, const String& field,
               int minVal, int maxVal, int step, int* preset,
               const String& tip = "");
```
Standard `<input type="number">` with min/max/step. Use this when you want keyboard entry instead of a slider.

#### Text and password

```cpp
void addText(const String& label, const String& field, String* ptr,
             const String& placeholder = "",
             const String& tip = "");
void addPassword(const String& label, const String& field, String* ptr,
                 const String& tip = "");
```
Bound to a `String*`. Value is committed on blur or Enter. Password field includes a "Show" checkbox to toggle visibility. URL-decoded automatically.

#### Text input with send button

```cpp
void addTextInput(const String& label, const String& field, String* ptr,
                  const String& placeholder = "", int maxLen = 63,
                  const String& buttonLabel = "Send", const String& tip = "");
```
A text field with an inline action button to the right. Pressing the button (or Enter) sends the value via AJAX and fires the text callback. Unlike `addText()`, value is only sent on explicit submit — not on blur. Use this for command inputs, chat fields, or search boxes where you want the user to confirm before sending. The button label defaults to "Send" but can be customized (e.g. "Go", "Search", "Submit").

#### Time

```cpp
void addTime(const String& label, const String& field, int* preset,
             bool includeSeconds = false,
             const String& tip = "");
```
HTML5 time input. Stored as an integer in `HHMM` format (e.g. `1430` = 14:30). If `includeSeconds = true`, the picker also shows seconds — but the integer encoding is still HHMM (seconds discarded).

#### Color picker

```cpp
void addColorPicker(const String& label, const String& field, int* preset,
                    const String& tip = "");
```
Native HTML5 color picker. The bound `int` is a 24-bit RGB value (`0xRRGGBB`).

#### Checkbox

```cpp
void addCheckbox(const String& label, const String& field, int* preset,
                 const String& tip = "");
```
Bound int is `0` (unchecked) or `1` (checked).

#### Radio group

```cpp
void addRadio(const String& label, const String& field,
              const String& options, int* preset,
              const String& tip = "");
```
Vertical radio button list. Same CSV format as dropdowns. Bound int is the zero-based selected index.

#### Hidden

```cpp
void addHidden(const String& field, int* preset);
```
Invisible field with no rendered UI. Useful for storing/retrieving an integer alongside the form (e.g. a saved scroll position) that should be persisted on Save but not shown.

#### Layout helpers

```cpp
void addSubheading(const String& text);
void addSeparator();
```
A bold subheading with an underline divides field groups visually. A separator is a thin horizontal line.

### Pages and navigation

```cpp
void addPage(const String& line1, const String& line2 = "");
```
Register a new sub-page. Subsequent `add*` calls bind to this page until the next `addPage()` call or a `setHomePage()` call. The home page is page `-1` and is implicit; you only call `addPage()` to add additional pages.

```cpp
void setHomePage();
```
Switch back to the home page so that subsequent `add*()` calls bind to it. Useful when you need to add fields or subheadings to the home page after creating sub-pages.

`line1` is the large header on that sub-page; `line2` is the smaller subtitle. The navigation button label on the home page uses `line2` if non-empty, otherwise `line1`.

When the home page has any of its own fields (added before any `addPage()` call), a "Save Settings" button appears at the bottom of the home page. If the home page has only navigation buttons (no fields), the Save button is suppressed.

```cpp
void setupForm() {
  setupForm.setTitle("My Device", "Settings v1.0");

  // Page 0: Network
  setupForm.addPage("My Device", "Network");
  setupForm.addText("SSID", "ssid", &xSsid);
  setupForm.addPassword("Password", "pw", &xPassword);

  // Page 1: Display
  setupForm.addPage("My Device", "Display");
  setupForm.addRange("Brightness", "br", 0, 9, &brightness);
  setupForm.addColorPicker("Accent", "col", &color);

  // Page 2: Diagnostics
  setupForm.addPage("My Device", "Diagnostics");
  setupForm.addCheckbox("Verbose Logging", "log", &verboseLog);
}
```

The home page now shows three nav buttons (Network, Display, Diagnostics) and no fields. Each sub-page has its own Save button + Back button.

### Action buttons

```cpp
void addActionButton(const String& label, const String& fieldName,
                     const String& confirmMessage = "");
```

Action buttons are always added to the **home page**, regardless of which sub-page is currently being built. They fire your change callback with `value=1` when clicked. They do **not** trigger the Save button to appear, so you can have action-only home pages (e.g. a setup page with only "Start" and "Cancel" buttons).

Two modes:

1. **Standard mode** (no confirm message): clicks send a regular AJAX field-change. Use this when the action returns control to the user (e.g. "Test Buzzer" — the device buzzes briefly, the user stays on the page).

2. **Confirm-and-clear mode** (`confirmMessage` non-empty): clicks fire a fire-and-forget AJAX call, then immediately replace the entire page with a full-screen confirmation overlay. Use this for actions that reboot the device — the AJAX response will never arrive, so don't wait for one. The overlay fades after 2 seconds.

```cpp
panel.addActionButton("Start",         "start",  "\u2713 Starting…");
panel.addActionButton("Factory Reset", "reset",  "Factory reset…");
panel.addActionButton("Test Buzzer",   "buzz");  // standard mode
```

### Tooltips

Pass a non-empty `tip` string as the last argument to any interactive field method:

```cpp
panel.addRange("Brightness", "br", 0, 9, &brightness,
               "0 = display off, 9 = full brightness. Mid-range is the most efficient.");
```

An info icon (ⓘ) appears next to the label. Tapping it opens a floating dark slate bubble with the tooltip text. The bubble auto-positions above or below the field based on available space. Tapping anywhere on the form (including the bubble itself) closes it. Only one tooltip can be open at a time.

### Callbacks

```cpp
void setOnChange(void (*cb)(const String& field, int value));
```
Fires every time an integer-bound field changes (dropdown, range, checkbox, radio, color picker, time, number, hidden, action button). The `value` parameter is the new value. Use the `field` name to dispatch:

```cpp
void onChange(const String& field, int v) {
  if      (field == "br")    applyBrightness(v);
  else if (field == "color") applyColor(v);
  else if (field == "start") commitAndReboot();
}
```

```cpp
void setOnTextChange(void (*cb)(const String& field, const String& value));
```
Fires when a text or password field is committed (blur or Enter). Use this for input validation or trimming:

```cpp
void onText(const String& field, const String& v) {
  if (field == "ssid") xSsid = v.trim();
}
```

```cpp
void setSaveCallback(void (*cb)());
```
Fires when the user clicks any "Save Settings" button. Persist your bound variables to NVS / EEPROM here. The browser shows the "✓ Settings Saved" overlay automatically (or a custom overlay if `setRebootOnSave(true)`).

### Sending messages from callbacks

```cpp
void showMessage(const String& text);
```

Inside any callback (change, text, save), call `panel.showMessage("My text")` to override the default response. The browser will show your text in the same animated overlay as "Settings Saved". Useful for:

- Validation errors: `panel.showMessage("Invalid SSID — must be 1–31 characters");`
- Success messages: `panel.showMessage("Buzzer test complete");`
- Warnings: `panel.showMessage("⚠ Schedule conflict in Period 2");`

The message is one-shot — it's cleared after being sent in the next AJAX response.

### Conditional fields

```cpp
void addConditionalDropDown(bool (*condition)(),
                             const String& label, const String& field,
                             const String& options, int* preset,
                             const String& tip = "");
void addConditionalSubheading(bool (*condition)(), const String& text);
```

These take a function pointer that returns `bool`. The field is rendered only if the condition returns true at the moment of page render. Useful for hiding settings that only apply in certain modes:

```cpp
bool isAdvancedMode() { return cfgUserMode == 2; }
panel.addConditionalDropDown(isAdvancedMode, "Debug Level", "dbg",
                              "Off, Errors, Verbose", &cfgDebugLevel);
```

(Only conditional dropdown and subheading are exposed today; the same pattern can be added to any field type if needed.)

## Configuration constants

Define these before `#include "WebPanel.h"` to override defaults:

```cpp
#define WP_MAX_OPTIONS         50    // max options in any single CSV
#define WP_MAX_PAGES           10    // max sub-pages
#define WP_HTML_BUFFER_SIZE  40960   // static render buffer (40 KB default)
#include "WebPanel.h"
```

The field array is now dynamically allocated on the heap. The default capacity is 80 fields. Call `panel.setMaxFields(N)` before any `add*()` calls to set a custom capacity — use a smaller value to save RAM on simple forms, or a larger value for complex multi-page layouts.

## Live updates vs Save

WebPanel sends every field change as an AJAX call **the moment the user releases the control**, before the user touches the Save button. This means:

- Your `int*` bound variables are **always up to date** with whatever is on the screen.
- Your `onChange` callback fires for every change — perfect for "live" runtime adjustment of brightness, colors, modes, etc.
- The Save button calls your `setSaveCallback()` to persist the current variable values to non-volatile storage.

This split lets you offer **two distinct UX modes** with the same library:

1. **Live tuning form** (e.g. runtime settings page on a device with WiFi): changes apply immediately so the user sees the effect on the device. Save persists. Don't reboot.

2. **Initial-setup form** (e.g. AP-mode WiFi configuration): user fills in fields, then clicks Start to commit and reboot into normal operation. The fields are still bound directly, so onChange fires per-field, but you only act on them at Save time. Use `setRebootOnSave(true)` to show the reboot overlay.

You can run both forms in the same project on the same WebPanel instance — just pick which fields to add based on the current mode.

## Limitations

- **One client at a time.** WebPanel uses synchronous `WiFiClient` reads. If two browsers fetch simultaneously, the second waits ~200 ms. Fine for the intended single-user device-config use case.
- **No HTTPS.** Plain HTTP only. The forms are intended for local/AP use; don't expose them on the public internet.
- **No file upload.** The library handles `GET /?field=...` and `GET /?save=1` only. There's no `POST` parser, no multipart, no file upload.
- **No JSON output.** Field values are bound to your variables; the library doesn't expose a JSON dump endpoint. Add one yourself if needed.
- **Static HTML buffer is fixed-size.** If your rendered page exceeds `WP_HTML_BUFFER_SIZE`, the overflow is silently truncated. Bump the buffer or split into more pages.
- **Field name parsing is positional.** AJAX requests must arrive in the exact form `/?field=NAME&value=N`. Don't add extra query parameters.

## License

MIT. See file headers.

---

*WebPanel is part of the Zev-7 nixie clock and WordClock-5 firmware projects.*
