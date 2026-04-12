# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

WebPanel is a self-contained Arduino library for ESP32 that generates responsive HTML/CSS/JS settings forms entirely from C++. No SPIFFS, no template files, no external HTTP framework. Variables are bound directly via `int*` / `String*` pointers and update via AJAX the moment the user releases a control.

Two source files: `WebPanel.h` (class definition, field enum, API) and `WebPanel.cpp` (field builders, HTML generation, HTTP handling, AJAX processing).

## Build

This is an Arduino library — no standalone build. It compiles as part of a sketch project:
- Place in the Arduino libraries folder or sketch directory
- `#include "WebPanel.h"` (or `#include <WebPanel.h>` if in libraries folder)
- Dependencies: ESP32 core only (`WiFi.h`, `WiFiClient.h`, `WiFiServer.h`, `mbedtls/base64.h`)
- No tests, no CI — tested through consuming projects (e.g. Zev-7)

## Architecture

### Request flow

`handleClient()` is called every `loop()` iteration. It accepts one TCP connection at a time from `WiFiServer`, reads the HTTP request, and dispatches:

1. `GET /?save=1` → `handleSave()` → fires user's save callback
2. `GET /?field=NAME&value=N` → `handleAjax()` → writes value to bound pointer, fires change callback
3. Everything else → `serveForm()` → renders full HTML page into static buffer, writes to socket

### HTML generation

`serveForm()` resets `_htmlPos = 0`, then builds a complete HTML document into `_htmlBuf` via `out()` append helpers: embedded CSS (~5 KB), field markup generated per-type (`genDropDown()`, `genRange()`, etc.), and embedded JS (~1 KB with AJAX helpers). The buffer is written to the client in one shot.

### Field system

All fields are stored in `WPField _fields[WP_MAX_FIELDS]`. Each field has a `WPFieldType` enum, label, field name (doubles as HTML id), bound pointer (`int*` or `String*`), optional tooltip text, and optional condition function pointer for conditional rendering. Fields are assigned to pages via `_currentPage` (set by `addPage()`).

16 field types: dropdown, dropdown-offset, dropdown-range, range, number, text, password, checkbox, radio, time, color picker, hidden, subheading, separator, page button, action button.

### Memory model

A single static `char*` buffer (`_htmlBuf`, default 40 KB) is heap-allocated once via `allocBuffer()` and shared across all WebPanel instances. **Must be allocated before WiFi.begin** to prevent ESP32 heap fragmentation — this is the library's most important usage constraint. The buffer is reused on every page render.

### Authentication

Optional HTTP Basic Auth via `setAuth(String* password)`. Uses ESP32's `mbedtls_base64_decode()` to decode credentials. The username is ignored — only the password is checked. Auth is enforced when `*password` is non-empty; disabled when empty.

## Key constants (user-overridable before #include)

- `WP_MAX_FIELDS 125` — total fields across all pages
- `WP_MAX_OPTIONS 50` — max items in any single dropdown/radio CSV
- `WP_MAX_PAGES 10` — max sub-pages (home page is implicit)
- `WP_HTML_BUFFER_SIZE 40960` — render buffer size in bytes

## Conventions

- Every `add*` method for interactive fields accepts an optional trailing `tip` parameter (tooltip text)
- Action buttons are always added to the home page regardless of current page context
- Options are passed as CSV strings (`"Off, Low, Medium, High"`) and parsed at render time into a stack array
- Text/password values are URL-decoded in `handleAjax()` via the private `urlDecode()` method
- The `showMessage()` method sets `_pendingMessage` which overrides the next AJAX response body (one-shot)
