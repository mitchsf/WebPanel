# WebPanel host renderer

Renders real WebPanel pages on a Windows PC (MSVC Build Tools 2022) against a
small Arduino shim, so a form's exact HTML size can be measured before it is
trusted to a no-PSRAM render buffer, and library changes can be proven not to
alter the HTML.

- `shim/` — Arduino.h (String with Arduino's layout, IPAddress, ESP, millis),
  fake WiFiClient/WiFiServer capturing one GET, mbedtls/base64, sys/socket.
- `host_main.cpp` — builds forms with the firmware's own add*() calls, feeds
  `GET /pageN`, prints `WebPanel::lastRenderLen()` / truncation, dumps each
  body to `$WPHOST_OUT/<title>.html` plus `/wp.css` and `/wp.js`.
- `build.bat` — vcvars64 + cl; run with `cmd.exe //c <win path>\build.bat`
  from Git Bash, then `./wphost.exe` (cmd does not find it by bare name).
- `compare.py` — proves a 1.4.0 render equals a 1.3.3 render with the CSS/JS
  lifted out (`before/` vs `after/`).

Numbers (2026-11-04): fixed chrome was 16,365 B per page before 1.4.0;
WordClock-1-4 Setup sub-page 28,644 B -> 13,080 B; /wp.css 10,421 B,
/wp.js 5,397 B.
