// Minimal Arduino shim so engine.cpp compiles and runs on the Mac for tests.
#pragma once
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#ifndef PI
#define PI 3.14159265358979f
#endif

using std::max;
using std::min;

template <class T, class L, class H>
static inline T constrain(T v, L lo, H hi) { return v < (T)lo ? (T)lo : v > (T)hi ? (T)hi : v; }

class String {
 public:
  std::string s;
  String() {}
  String(const char* c) : s(c ? c : "") {}
  String(const std::string& x) : s(x) {}
  String(char c) : s(1, c) {}
  const char* c_str() const { return s.c_str(); }
  unsigned length() const { return s.size(); }
  char operator[](unsigned i) const { return s[i]; }
  bool operator==(const String& o) const { return s == o.s; }
  bool operator==(const char* o) const { return s == o; }
  String& operator+=(const String& o) { s += o.s; return *this; }
  String& operator+=(const char* o) { s += o; return *this; }
  String& operator+=(char c) { s += c; return *this; }
  friend String operator+(const String& a, const String& b) { return String(a.s + b.s); }
  friend String operator+(const String& a, const char* b) { return String(a.s + b); }
  friend String operator+(const String& a, char c) { return String(a.s + c); }
  int indexOf(char c, unsigned from = 0) const { auto p = s.find(c, from); return p == std::string::npos ? -1 : (int)p; }
  String substring(unsigned a) const { return a >= s.size() ? String() : String(s.substr(a)); }
  String substring(unsigned a, unsigned b) const { return String(s.substr(a, b - a)); }
  void trim() {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    s = a == std::string::npos ? "" : s.substr(a, b - a + 1);
  }
  void replace(const char* f, const char* t) {
    std::string F(f), T(t);
    for (size_t p = 0; (p = s.find(F, p)) != std::string::npos; p += T.size()) s.replace(p, F.size(), T);
  }
  bool equalsIgnoreCase(const String& o) const {
    if (o.s.size() != s.size()) return false;
    for (size_t i = 0; i < s.size(); i++) if (tolower(s[i]) != tolower(o.s[i])) return false;
    return true;
  }
  int toInt() const { return atoi(s.c_str()); }
};

class Print {
 public:
  virtual ~Print() {}
  virtual size_t write(uint8_t c) { return fputc(c, stdout) != EOF; }
  size_t printf(const char* f, ...) {
    char b[512];
    va_list ap;
    va_start(ap, f);
    int n = vsnprintf(b, sizeof b, f, ap);
    va_end(ap);
    for (int i = 0; i < n && i < (int)sizeof b; i++) write(b[i]);
    return n;
  }
  size_t print(const char* s) { size_t n = 0; while (*s) n += write(*s++); return n; }
  size_t println(const char* s = "") { return print(s) + write('\n'); }
};

typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(x) (void)(x)
#define portEXIT_CRITICAL(x) (void)(x)

uint32_t millis();

#ifdef CHORDBOX_HOST_GPIO  // pins, for the keypad test only (test_input.cpp defines them)
#define INPUT 0x01
#define OUTPUT 0x03
#define INPUT_PULLUP 0x05
#define INPUT_PULLDOWN 0x09
#define LOW 0
#define HIGH 1
#define CHANGE 3
void pinMode(uint8_t pin, uint8_t mode);
void digitalWrite(uint8_t pin, uint8_t val);
int digitalRead(uint8_t pin);
void delayMicroseconds(unsigned us);
int digitalPinToInterrupt(int pin);
void attachInterrupt(int irq, void (*fn)(), int mode);
void noInterrupts();
void interrupts();
#endif
