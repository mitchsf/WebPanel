// Fake WiFi / WiFiClient / WiFiServer: one queued request, captured response.
#pragma once
#include "Arduino.h"
#include <string>

enum wl_status_t { WL_IDLE_STATUS = 0, WL_CONNECTED = 3, WL_DISCONNECTED = 6 };
enum WiFiMode_t { WIFI_OFF = 0, WIFI_STA = 1, WIFI_AP = 2, WIFI_AP_STA = 3 };

struct HostCapture {
  std::string request;   // bytes the "browser" sends
  std::string response;  // bytes WebPanel writes back
  size_t rpos = 0;
  bool pending = false;  // a client is waiting to be accepted
};
extern HostCapture g_http;

class WiFiClient {
 public:
  bool open = false;
  WiFiClient() {}
  explicit WiFiClient(bool o) : open(o) {}
  operator bool() const { return open; }
  int fd() const { return 3; }
  void setNoDelay(bool) {}
  void setTimeout(unsigned long) {}
  bool connected() const { return open; }
  int available() const { return open ? (int)(g_http.request.size() - g_http.rpos) : 0; }
  int read() { if (!available()) return -1; return (unsigned char)g_http.request[g_http.rpos++]; }
  int read(uint8_t* buf, size_t n) { size_t i = 0; while (i < n && available()) buf[i++] = (uint8_t)read(); return (int)i; }
  int peek() { return available() ? (unsigned char)g_http.request[g_http.rpos] : -1; }
  size_t write(const uint8_t* b, size_t n) { if (open) g_http.response.append((const char*)b, n); return n; }
  size_t write(const char* b, size_t n) { return write((const uint8_t*)b, n); }
  size_t write(uint8_t c) { return write(&c, 1); }
  size_t print(const char* c) { return write((const uint8_t*)c, strlen(c)); }
  size_t print(const String& s) { return print(s.c_str()); }
  size_t print(char c) { return write((uint8_t)c); }
  size_t print(int v) { return print(String(v)); }
  size_t print(unsigned int v) { return print(String(v)); }
  size_t print(long v) { return print(String(v)); }
  size_t print(unsigned long v) { return print(String(v)); }
  size_t print(size_t v) { return print(String((unsigned long)v)); }
  size_t println(const char* c) { return print(c) + print("\r\n"); }
  size_t println(const String& s) { return print(s) + print("\r\n"); }
  size_t println() { return print("\r\n"); }
  void flush() {}
  void stop() { open = false; }
};

class WiFiServer {
 public:
  explicit WiFiServer(int = 80) {}
  void begin() {}
  bool hasClient() { return g_http.pending; }
  WiFiClient accept() { g_http.pending = false; return WiFiClient(true); }
  WiFiClient available() { return accept(); }
};

struct WiFiClass {
  wl_status_t status() { return WL_CONNECTED; }
  IPAddress softAPIP() { return IPAddress(192, 168, 4, 1); }
  IPAddress localIP() { return IPAddress(192, 168, 4, 58); }
  int RSSI() { return -60; }
  WiFiMode_t getMode() { return WIFI_AP; }
};
extern WiFiClass WiFi;
