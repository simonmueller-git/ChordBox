// In-memory Preferences stand-in for host tests.
#pragma once
#include <map>
#include "Arduino.h"

extern std::map<std::string, std::string> g_prefs;

class Preferences {
  std::string ns;
 public:
  bool begin(const char* n, bool) { ns = n; return true; }
  void end() {}
  size_t getBytesLength(const char* k) { auto it = g_prefs.find(ns + "/" + k); return it == g_prefs.end() ? 0 : it->second.size(); }
  size_t getBytes(const char* k, void* b, size_t n) {
    auto it = g_prefs.find(ns + "/" + k);
    if (it == g_prefs.end()) return 0;
    memcpy(b, it->second.data(), std::min(n, it->second.size()));
    return n;
  }
  size_t putBytes(const char* k, const void* b, size_t n) { g_prefs[ns + "/" + k] = std::string((const char*)b, n); return n; }
};
