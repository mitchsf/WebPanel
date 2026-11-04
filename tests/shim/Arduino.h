// Minimal Arduino/ESP32 shim so WebPanel.cpp compiles and renders on a PC.
// String mirrors Arduino's layout (pointer + lengths), so a calloc-zeroed
// String is a valid empty string — WebPanel relies on that for its field table.
#pragma once
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <string>
#include <algorithm>

typedef uint8_t byte;
typedef bool boolean;
#define PROGMEM
#define PSTR(x) (x)
#define F(x) (x)
#define IRAM_ATTR
#define pgm_read_byte(p) (*(const uint8_t*)(p))
#define pgm_read_word(p) (*(const uint16_t*)(p))

template <class T> T constrain(T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline int constrain(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
#ifndef min
template <class T> T min(T a, T b) { return a < b ? a : b; }
template <class T> T max(T a, T b) { return a > b ? a : b; }
#endif

extern unsigned long g_millis;
inline unsigned long millis() { return g_millis += 1; }
inline void delay(unsigned long ms) { g_millis += ms; }
inline void yield() {}
inline char* itoa(int v, char* b, int base) { _itoa_s(v, b, 34, base); return b; }
inline char* ltoa(long v, char* b, int base) { _ltoa_s(v, b, 34, base); return b; }
inline char* utoa(unsigned v, char* b, int base) { _ultoa_s(v, b, 34, base); return b; }

class String {
  char* buffer = nullptr;
  unsigned int capacity = 0;
  unsigned int len = 0;
  void setLen(unsigned int n) { len = n; if (buffer) buffer[len] = 0; }
  bool grow(unsigned int n) {
    if (buffer && capacity >= n) return true;
    char* nb = (char*)realloc(buffer, n + 1);
    if (!nb) return false;
    if (!buffer) nb[0] = 0;
    buffer = nb; capacity = n;
    return true;
  }
  void assign(const char* c, unsigned int n) {
    if (!c) { setLen(0); return; }
    if (grow(n)) { memmove(buffer, c, n); setLen(n); }
  }
  void append(const char* c, unsigned int n) {
    if (!c || n == 0) return;
    if (grow(len + n)) { memcpy(buffer + len, c, n); setLen(len + n); }
  }
 public:
  String() {}
  String(const char* c) { assign(c, c ? (unsigned)strlen(c) : 0); }
  String(const char* c, unsigned int n) { assign(c, n); }
  String(const std::string& c) { assign(c.data(), (unsigned)c.size()); }
  String(const String& o) { assign(o.buffer, o.len); }
  String(String&& o) noexcept : buffer(o.buffer), capacity(o.capacity), len(o.len) { o.buffer = nullptr; o.capacity = o.len = 0; }
  String(char c) { assign(&c, 1); }
  String(int v) { char b[34]; _itoa_s(v, b, sizeof b, 10); assign(b, (unsigned)strlen(b)); }
  String(unsigned int v) { char b[34]; _ultoa_s(v, b, sizeof b, 10); assign(b, (unsigned)strlen(b)); }
  String(long v) { char b[34]; _ltoa_s(v, b, sizeof b, 10); assign(b, (unsigned)strlen(b)); }
  String(unsigned long v) { char b[34]; _ultoa_s(v, b, sizeof b, 10); assign(b, (unsigned)strlen(b)); }
  String(long long v) { char b[34]; _i64toa_s(v, b, sizeof b, 10); assign(b, (unsigned)strlen(b)); }
  String(unsigned long long v) { char b[34]; _ui64toa_s(v, b, sizeof b, 10); assign(b, (unsigned)strlen(b)); }
  String(float v) { char b[40]; snprintf(b, sizeof b, "%.2f", v); assign(b, (unsigned)strlen(b)); }
  String(double v) { char b[40]; snprintf(b, sizeof b, "%.2f", v); assign(b, (unsigned)strlen(b)); }
  String(int v, int base) { char b[34]; _itoa_s(v, b, sizeof b, base); assign(b, (unsigned)strlen(b)); }
  ~String() { free(buffer); }

  String& operator=(const String& o) { if (this != &o) assign(o.buffer, o.len); return *this; }
  String& operator=(String&& o) noexcept { if (this != &o) { free(buffer); buffer = o.buffer; capacity = o.capacity; len = o.len; o.buffer = nullptr; o.capacity = o.len = 0; } return *this; }
  String& operator=(const char* c) { assign(c, c ? (unsigned)strlen(c) : 0); return *this; }
  String& operator+=(const String& o) { append(o.buffer, o.len); return *this; }
  String& operator+=(const char* c) { if (c) append(c, (unsigned)strlen(c)); return *this; }
  String& operator+=(char c) { append(&c, 1); return *this; }
  String& operator+=(int v) { return *this += String(v); }
  String& operator+=(unsigned int v) { return *this += String(v); }
  String& operator+=(long v) { return *this += String(v); }
  String& operator+=(unsigned long v) { return *this += String(v); }
  bool concat(const String& o) { append(o.buffer, o.len); return true; }
  bool concat(const char* c) { if (c) append(c, (unsigned)strlen(c)); return true; }
  bool concat(char c) { append(&c, 1); return true; }

  friend String operator+(const String& a, const String& b) { String r(a); r += b; return r; }
  friend String operator+(const String& a, const char* b) { String r(a); r += b; return r; }
  friend String operator+(const char* a, const String& b) { String r(a); r += b; return r; }
  friend String operator+(const String& a, char b) { String r(a); r += b; return r; }
  friend String operator+(const String& a, int b) { String r(a); r += b; return r; }
  friend String operator+(const String& a, unsigned int b) { String r(a); r += b; return r; }
  friend String operator+(const String& a, long b) { String r(a); r += b; return r; }
  friend String operator+(const String& a, unsigned long b) { String r(a); r += b; return r; }

  int compareTo(const String& o) const {
    if (!buffer || !o.buffer) return (int)len - (int)o.len;
    return strcmp(buffer, o.buffer);
  }
  bool operator==(const String& o) const { return len == o.len && (len == 0 || memcmp(buffer, o.buffer, len) == 0); }
  bool operator==(const char* c) const { return equals(c); }
  bool operator!=(const String& o) const { return !(*this == o); }
  bool operator!=(const char* c) const { return !equals(c); }
  bool operator<(const String& o) const { return compareTo(o) < 0; }
  bool equals(const String& o) const { return *this == o; }
  bool equals(const char* c) const { if (!c) return len == 0; return strlen(c) == len && (len == 0 || memcmp(buffer, c, len) == 0); }
  bool equalsIgnoreCase(const String& o) const { return len == o.len && (len == 0 || _stricmp(buffer, o.buffer) == 0); }

  char operator[](unsigned int i) const { return i < len ? buffer[i] : 0; }
  char& operator[](unsigned int i) { static char dummy; return i < len ? buffer[i] : (dummy = 0, dummy); }
  char charAt(unsigned int i) const { return (*this)[i]; }
  void setCharAt(unsigned int i, char c) { if (i < len) buffer[i] = c; }
  unsigned int length() const { return len; }
  bool isEmpty() const { return len == 0; }
  const char* c_str() const { return buffer ? buffer : ""; }
  void reserve(unsigned int n) { grow(n); }
  void toCharArray(char* b, unsigned int n) const { snprintf(b, n, "%s", c_str()); }
  void getBytes(unsigned char* b, unsigned int n) const { toCharArray((char*)b, n); }

  int indexOf(char c, unsigned int from = 0) const {
    if (from >= len) return -1;
    const char* p = (const char*)memchr(buffer + from, c, len - from);
    return p ? (int)(p - buffer) : -1;
  }
  int indexOf(const char* c, unsigned int from = 0) const {
    if (!c || from > len) return -1;
    const char* p = strstr(c_str() + from, c);
    return p ? (int)(p - c_str()) : -1;
  }
  int indexOf(const String& c, unsigned int from = 0) const { return indexOf(c.c_str(), from); }
  int lastIndexOf(char c) const { for (int i = (int)len - 1; i >= 0; i--) if (buffer[i] == c) return i; return -1; }
  int lastIndexOf(const String& c) const {
    int r = -1, p = 0;
    while ((p = indexOf(c, p)) >= 0) { r = p; p++; }
    return r;
  }
  int lastIndexOf(const char* c) const { return lastIndexOf(String(c)); }
  String substring(unsigned int a) const { return a >= len ? String() : String(buffer + a, len - a); }
  String substring(unsigned int a, unsigned int b) const {
    if (a > b) std::swap(a, b);
    if (a >= len) return String();
    if (b > len) b = len;
    return String(buffer + a, b - a);
  }
  bool startsWith(const String& p) const { return p.len <= len && (p.len == 0 || memcmp(buffer, p.buffer, p.len) == 0); }
  bool startsWith(const char* p) const { return startsWith(String(p)); }
  bool endsWith(const String& p) const { return p.len <= len && (p.len == 0 || memcmp(buffer + len - p.len, p.buffer, p.len) == 0); }
  bool endsWith(const char* p) const { return endsWith(String(p)); }
  void trim() {
    if (!len) return;
    unsigned int a = 0, b = len;
    while (a < b && isspace((unsigned char)buffer[a])) a++;
    while (b > a && isspace((unsigned char)buffer[b - 1])) b--;
    if (a) memmove(buffer, buffer + a, b - a);
    setLen(b - a);
  }
  void replace(char a, char b) { for (unsigned int i = 0; i < len; i++) if (buffer[i] == a) buffer[i] = b; }
  void replace(const String& a, const String& b) {
    if (!a.len || !len) return;
    std::string s(c_str());
    size_t p = 0;
    while ((p = s.find(a.c_str(), p)) != std::string::npos) { s.replace(p, a.len, b.c_str()); p += b.len; }
    assign(s.data(), (unsigned)s.size());
  }
  void replace(const char* a, const char* b) { replace(String(a), String(b)); }
  void remove(unsigned int i) { if (i < len) setLen(i); }
  void remove(unsigned int i, unsigned int n) {
    if (i >= len) return;
    if (n > len - i) n = len - i;
    memmove(buffer + i, buffer + i + n, len - i - n);
    setLen(len - n);
  }
  void toLowerCase() { for (unsigned int i = 0; i < len; i++) buffer[i] = (char)tolower((unsigned char)buffer[i]); }
  void toUpperCase() { for (unsigned int i = 0; i < len; i++) buffer[i] = (char)toupper((unsigned char)buffer[i]); }
  long toInt() const { return atol(c_str()); }
  float toFloat() const { return (float)atof(c_str()); }
  double toDouble() const { return atof(c_str()); }
};

class IPAddress {
 public:
  uint8_t o[4] = {192, 168, 4, 1};
  IPAddress() {}
  IPAddress(uint8_t a, uint8_t b, uint8_t c, uint8_t d) { o[0] = a; o[1] = b; o[2] = c; o[3] = d; }
  String toString() const { char b[20]; snprintf(b, sizeof b, "%u.%u.%u.%u", o[0], o[1], o[2], o[3]); return String(b); }
  operator uint32_t() const { return (uint32_t)o[0] | ((uint32_t)o[1] << 8) | ((uint32_t)o[2] << 16) | ((uint32_t)o[3] << 24); }
};

struct EspClass {
  uint32_t getFreeHeap() { return 200000; }
  uint32_t getMinFreeHeap() { return 150000; }
  uint32_t getMaxAllocHeap() { return 100000; }
  uint32_t getFreePsram() { return 0; }
  uint32_t getPsramSize() { return 0; }
  uint32_t getHeapSize() { return 320000; }
  uint64_t getEfuseMac() { return 0x1234567890ABULL; }
  void restart() {}
};
extern EspClass ESP;
inline bool psramFound() { return false; }
inline void* ps_malloc(size_t n) { return malloc(n); }
inline void* ps_calloc(size_t n, size_t m) { return calloc(n, m); }

#define HEX 16
#define DEC 10
