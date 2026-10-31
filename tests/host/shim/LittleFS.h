// In-memory LittleFS stand-in for host tests.
#pragma once
#include <map>
#include "Arduino.h"

extern std::map<std::string, std::string> g_files;

class File : public Print {
 public:
  std::string path, buf;
  size_t pos = 0;
  bool ok = false, writing = false;
  operator bool() const { return ok; }
  int available() { return (int)(buf.size() - pos); }
  int read() { return pos < buf.size() ? (uint8_t)buf[pos++] : -1; }
  size_t read(uint8_t* b, size_t n) { size_t k = 0; while (k < n && pos < buf.size()) b[k++] = buf[pos++]; return k; }
  size_t write(const uint8_t* b, size_t n) { buf.append((const char*)b, n); return n; }
  String readStringUntil(char t) {
    std::string out;
    while (pos < buf.size() && buf[pos] != t) out += buf[pos++];
    if (pos < buf.size()) pos++;
    return String(out);
  }
  size_t write(uint8_t c) override { buf += (char)c; return 1; }
  void close() { if (writing) g_files[path] = buf; ok = false; }
};

struct LittleFSClass {
  bool begin(bool) { return true; }
  File open(const char* p, const char* mode) {
    File f;
    f.path = p;
    if (mode[0] == 'w') { f.ok = true; f.writing = true; return f; }
    auto it = g_files.find(p);
    if (it == g_files.end()) return f;
    f.buf = it->second;
    f.ok = true;
    return f;
  }
};
extern LittleFSClass LittleFS;
