// web.cpp — the phone / iPad page, served by the ESP32 itself (see web.h)
//
// Everything runs in one task on core 0 (with the screen); the music loop on
// core 1 never waits for it. Commands from the page become ordinary events
// through postEvent(), so the phone can do nothing the keypad couldn't.

#include "bigmem.h"
#include "web.h"

#if defined(ARDUINO_ARCH_ESP32)

#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoOTA.h>
#include "config.h"
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <WiFi.h>
#include "app.h"
#include "bass.h"
#include "engine.h"
#include "platform.h"
#include "input.h"
#include "types.h"
#include "ui.h"
#include "web_page.h"

namespace {

struct Creds { char ssid[33]; char pass[65]; } creds;
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
char statusBuf[20] = "STARTING";
char noticeBuf[32] = "";
bool noticeNew = false;
uint32_t rebootAt = 0;
bool hotspot = false, ready = false;
volatile uint8_t phones = 0;
char urlBuf[32] = "";

void setStatus(const char* s) {
  portENTER_CRITICAL(&mux);
  strlcpy(statusBuf, s, sizeof statusBuf);
  portEXIT_CRITICAL(&mux);
}

void notice(const char* s) {
  portENTER_CRITICAL(&mux);
  strlcpy(noticeBuf, s, sizeof noticeBuf);
  noticeNew = true;
  portEXIT_CRITICAL(&mux);
}

void loadCreds() {
  memset(&creds, 0, sizeof creds);
  File f = LittleFS.open("/wifi.bin", "r");
  if (!f) return;
  if (f.read((uint8_t*)&creds, sizeof creds) != sizeof creds) memset(&creds, 0, sizeof creds);
  f.close();
}

void post(EvType t, int a = 0, int b = 0) {
  Event e{t, (uint8_t)a, (int16_t)b, 0};
  postEvent(e);
}

// one command line from the page: the same vocabulary as the serial console
void command(char* s) {
  char* v[4] = {};
  int n = 0;
  for (char* t = strtok(s, " "); t && n < 4; t = strtok(nullptr, " ")) v[n++] = t;
  if (!n) return;
  if (!strcmp(v[0], "pad") && n >= 3) {
    int p = atoi(v[1]);
    if (p >= 0 && p < 32) post(atoi(v[2]) ? EV_PAD_DOWN : EV_PAD_UP, p);
  } else if (!strcmp(v[0], "act") && n >= 2) {
    int id = app::actionIndex(String(v[1]));
    if (id >= 0) post(EV_ACTION, id, n >= 3 ? atoi(v[2]) : 0);
  } else if (!strcmp(v[0], "set") && n >= 3) {
    int p = engine::paramIndex(String(v[1]));
    if (p >= 0) post(EV_SET, p, atoi(v[2]));
  } else if (!strcmp(v[0], "bset") && n >= 3) {
    int p = bass::paramIndex(String(v[1]));
    if (p >= 0) post(EV_BSET, p, atoi(v[2]));
  } else if (!strcmp(v[0], "turn") && n >= 2) {
    post(EV_TURN, 0, atoi(v[1]));
  } else if (!strcmp(v[0], "push") && n >= 2) {
    post(EV_PUSH_TURN, 0, atoi(v[1]));
  } else if (!strcmp(v[0], "click")) {
    post(EV_CLICK);
  } else if (!strcmp(v[0], "turn2") && n >= 2) {   // the second knob
    post(EV_TURN2, 0, atoi(v[1]));
  } else if (!strcmp(v[0], "push2") && n >= 2) {
    post(EV_PUSH_TURN2, 0, atoi(v[1]));
  } else if (!strcmp(v[0], "click2")) {
    post(EV_CLICK2);
  } else if (!strcmp(v[0], "btn") && n >= 3) {     // btn K 1|0: an OLED button (K 1-4)
    int k = atoi(v[1]);
    if (k >= 1 && k <= 4) post(EV_BTN, k - 1, atoi(v[2]) ? 1 : 0);
  }
}

// ---- state as JSON, built into one buffer
char out[6144];
size_t pos = 0;

void put(const char* fmt, ...) {
  if (pos >= sizeof out) return;
  va_list ap;
  va_start(ap, fmt);
  int k = vsnprintf(out + pos, sizeof out - pos, fmt, ap);
  va_end(ap);
  if (k > 0) pos += k;
}

void str(const char* s) {                      // a quoted, escaped JSON string
  put("\"");
  for (; *s && pos < sizeof out - 4; s++) {
    if (*s == '"' || *s == '\\') out[pos++] = '\\';
    out[pos++] = (*s >= 32 && *s < 127) ? *s : ' ';
  }
  put("\"");
}

UiModel* mP = nullptr;                           // in PSRAM
uint8_t scenes = 0;

const char* buildState() {
  UiModel& m = *bigOnce(mP);
  modelRead(m);
  pos = 0;
  put("{\"mode\":%d,\"ov\":%d,\"key\":", m.mode, m.overlay);
  str(m.key);
  put(",\"scale\":");
  str(m.scale);
  put(",\"ext\":%d,\"run\":%d,\"bpm\":%.1f,\"step\":%lu,\"move\":%d,\"wifi\":", m.clk.ext, m.clk.running, m.clk.bpm10 / 10.0,
      (unsigned long)m.clk.step, m.moveUp);
  str(statusBuf);
  put(",\"toast\":");
  str(m.toastUntil > millis() ? m.toast : "");
  const ChordView& c = m.chord;
  put(",\"chord\":{\"bank\":");
  str(c.bank);
  put(",\"page\":%d,\"pages\":%d,\"played\":%d,\"sym\":", c.page, c.pages, c.played);
  str(c.sym);
  put(",\"rn\":");
  str(c.rn);
  put(",\"tag\":");
  str(c.rowTag);
  put(",\"notes\":[");
  for (int i = 0; i < c.nNotes; i++) put(i ? ",%d" : "%d", c.notes[i]);
  put("],\"latch\":%d,\"motion\":", c.latch);
  str(c.motion);
  put(",\"rpt\":%d,\"echo\":%d,\"loop\":%d,\"loopN\":%d,\"loopPos\":%d,\"pads\":[", c.repeat, c.echo, c.loopState, c.loopN,
      c.loopPos);
  for (int i = 0; i < 16; i++) {
    put(i ? ",[" : "[");
    str(c.pad[i].root);
    put(",");
    str(c.pad[i].qual);
    put(",");
    str(c.pad[i].rn);
    put(",%d]", c.pad[i].flags);
  }
  put("],\"fav\":[%d,%d,%d,%d]}", c.rowFav[0], c.rowFav[1], c.rowFav[2], c.rowFav[3]);
  const BassView& b = m.bass;
  put(",\"bass\":{\"bank\":");
  str(b.bank);
  put(",\"page\":%d,\"pages\":%d,\"gen\":%d,\"name\":", b.page, b.pages, b.genPage);
  str(b.feel);                                    // what the line is, not its name
  put(",\"play\":%d,\"queued\":%d,\"muted\":%d,\"follow\":", b.playPad, b.queuedPad, b.muted);
  str(b.follow);
  put(",\"chord\":");
  str(b.chordName);
  put(",\"note\":");
  str(b.noteName);
  put(",\"pads\":[");
  for (int i = 0; i < 16; i++) {
    put(i ? ",[" : "[");
    str(b.pad[i].feel);
    put(",%d,%u,%u]", b.pad[i].flags, b.pad[i].onsets, b.pad[i].accents);
  }
  put("]}");
  using namespace engine;
  put(",\"p\":{\"STRM\":%d,\"HUM\":%d,\"SOFT\":%d,\"VOIC\":%d,\"ARP\":%d,\"ARATE\":%d,\"RHY\":%d,\"ECHO\":%d,"
      "\"RPT\":%d,\"VRND\":%d,\"WEIGHT\":%d,\"SWING\":%d,\"DENS\":%d,\"TEMPO\":%d,\"BOCT\":%d,\"BLEN\":%d}",
      paramGet(P_STRM), paramGet(P_HUM), paramGet(P_SOFT), paramGet(P_VOIC), paramGet(P_ARP), paramGet(P_ARATE),
      paramGet(P_RHY), paramGet(P_ECHO), paramGet(P_RPT), paramGet(P_VRND), paramGet(P_WEIGHT),
      bass::paramGet(bass::BP_SWING), bass::paramGet(bass::BP_DENS), bass::paramGet(bass::BP_TEMPO),
      bass::paramGet(bass::BP_OCT), bass::paramGet(bass::BP_LEN));
  put(",\"ssid\":");
  str(hotspot ? "" : creds.ssid);
  put(",\"scenes\":%d,\"scene\":%d}", scenes, app::sceneSelected());
  return out;
}

void onWs(AsyncWebSocket*, AsyncWebSocketClient*, AwsEventType type, void* arg, uint8_t* data, size_t len) {
  if (type != WS_EVT_DATA) return;
  AwsFrameInfo* info = (AwsFrameInfo*)arg;
  if (!info->final || info->index || info->len != len || info->opcode != WS_TEXT || len > 60) return;
  char line[64];
  memcpy(line, data, len);
  line[len] = 0;
  command(line);
}

void routes() {
  ws.onEvent(onWs);
  server.addHandler(&ws);
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* r) {
    AsyncWebServerResponse* res = r->beginResponse(200, "text/html", (const uint8_t*)INDEX_HTML, strlen(INDEX_HTML));
    res->addHeader("Cache-Control", "no-store");
    r->send(res);
  });
  server.on("/scan", HTTP_GET, [](AsyncWebServerRequest* r) {
    int n = WiFi.scanComplete();
    if (n == WIFI_SCAN_FAILED) WiFi.scanNetworks(true);
    String j = "[";
    for (int i = 0; n > 0 && i < n; i++) {
      String s = WiFi.SSID(i);
      s.replace("\"", "");
      j += (i ? ",\"" : "\"") + s + "\"";
    }
    j += "]";
    if (n >= 0) {
      WiFi.scanDelete();
      WiFi.scanNetworks(true);
    }
    r->send(200, "application/json", j);
  });
  server.on("/wifi", HTTP_POST, [](AsyncWebServerRequest* r) {
    if (!r->hasParam("ssid", true)) {
      r->send(400, "text/plain", "missing ssid");
      return;
    }
    String ssid = r->getParam("ssid", true)->value();
    String pass = r->hasParam("pass", true) ? r->getParam("pass", true)->value() : "";
    web::setCredentials(ssid.c_str(), pass.c_str());
    r->send(200, "text/plain", "saved: ChordBox restarts and joins " + ssid);
    rebootAt = millis() + 1500;
  });
  server.on("/diag", HTTP_GET, [](AsyncWebServerRequest* r) {   // what the knob and KEY0 are doing
    AsyncResponseStream* res = r->beginResponseStream("text/plain");
    input::debug(*res);
    UiModel& m = *bigOnce(mP);
    modelRead(m);
    res->printf("internal heap free %u  psram free %u\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    res->printf("screen model: mode %d  overlay %d  seq %lu   heap %u\n", m.mode, m.overlay, (unsigned long)m.seq,
                ESP.getFreeHeap());
    ui::oledCmd("", *res);
    r->send(res);
  });
  server.on("/oled", HTTP_GET, [](AsyncWebServerRequest* r) {   // the OLED's picture, # = lit
    AsyncResponseStream* res = r->beginResponseStream("text/plain");
    ui::oledShot(*res);
    r->send(res);
  });
  server.on("/shot", HTTP_GET, [](AsyncWebServerRequest* r) {   // the framebuffer, as `shot` sends it
    struct Buf : Print {
      uint8_t* b;
      size_t n = 0, cap;
      size_t write(uint8_t c) override { if (n < cap) b[n++] = c; return 1; }
    };
    static uint8_t* mem = (uint8_t*)ps_malloc(160000);
    if (!mem) {
      r->send(500, "text/plain", "no memory");
      return;
    }
    Buf buf;
    buf.b = mem;
    buf.cap = 160000;
    ui::shot(buf);
    r->send(200, "application/octet-stream", mem, buf.n);
  });
  server.onNotFound([](AsyncWebServerRequest* r) { r->redirect("/"); });
}

void task(void*) {
  loadCreds();
  WiFi.persistent(false);
  WiFi.setHostname("chordbox");
  bool joined = false;
  if (creds.ssid[0]) {
    setStatus("JOINING");
    WiFi.mode(WIFI_STA);
    WiFi.enableIPv6();                           // phones ask for chordbox.local over IPv6 first
    WiFi.begin(creds.ssid, creds.pass);
    for (int i = 0; i < 150 && WiFi.status() != WL_CONNECTED; i++) vTaskDelay(pdMS_TO_TICKS(100));
    joined = WiFi.status() == WL_CONNECTED;
  }
  if (joined) {
    setStatus(WiFi.localIP().toString().c_str());
    notice("PHONE: MORE > PHONE LINK");
  } else {                                       // travelling: its own hotspot
    WiFi.disconnect(true);
    WiFi.mode(WIFI_AP);
    WiFi.softAP("ChordBox", HOTSPOT_PASSWORD);
    WiFi.softAPenableIPv6();
    hotspot = true;
    setStatus(WiFi.softAPIP().toString().c_str());
    notice("PHONE: WIFI ChordBox");
  }
  portENTER_CRITICAL(&mux);
  snprintf(urlBuf, sizeof urlBuf, "http://%s", statusBuf);
  ready = true;
  portEXIT_CRITICAL(&mux);
  MDNS.begin("chordbox");
  MDNS.addService("http", "tcp", 80);
  routes();
  server.begin();
  // updates over Wi-Fi: ./build.sh esp32 ota (password in local_config.h)
  const bool otaOn = OTA_PASSWORD[0] != 0;
  if (otaOn) {
    ArduinoOTA.setHostname("chordbox");
    ArduinoOTA.setPassword(OTA_PASSWORD);
    ArduinoOTA.setMdnsEnabled(false);
    ArduinoOTA.onStart([]() { notice("UPDATING OVER WIFI"); });
    ArduinoOTA.begin();
  }
  WiFi.scanNetworks(true);                       // ready for the setup list

  uint32_t lastSeq = 0, lastPush = 0, lastTidy = 0, lastScenes = 0;
  bool wasJoined = joined;
  for (;;) {
    uint32_t now = millis();
    if (rebootAt && now > rebootAt) ESP.restart();
    if (otaOn) ArduinoOTA.handle();
    phones = ws.count();
    if (now - lastTidy > 1000) {
      lastTidy = now;
      ws.cleanupClients();
      if (!hotspot) {
        bool j = WiFi.status() == WL_CONNECTED;
        if (j != wasJoined) {
          setStatus(j ? WiFi.localIP().toString().c_str() : "RECONNECTING");
          if (j) {
            portENTER_CRITICAL(&mux);
            snprintf(urlBuf, sizeof urlBuf, "http://%s", statusBuf);
            portEXIT_CRITICAL(&mux);
          }
        }
        wasJoined = j;
      }
    }
    if (ws.count()) {
      if (now - lastScenes > 3000) {             // which scenes are saved (flash)
        lastScenes = now;
        scenes = app::scenesUsed();
      }
      static UiModel* peekP = nullptr;
      UiModel& peek = *bigOnce(peekP);
      modelRead(peek);
      bool changed = peek.seq != lastSeq;
      if ((changed && now - lastPush >= 80) || now - lastPush >= 1000) {
        if (ws.availableForWriteAll()) {
          lastSeq = peek.seq;
          lastPush = now;
          ws.textAll(buildState());
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(30));
  }
}

}  // namespace

void web::begin() { xTaskCreatePinnedToCore(task, "web", 12288, nullptr, 1, nullptr, 0); }

const char* web::status() { return statusBuf; }   // short, rarely changes: read as is

bool web::takeNotice(char* dst, size_t cap) {
  bool got = false;
  portENTER_CRITICAL(&mux);
  if (noticeNew) {
    strlcpy(dst, noticeBuf, cap);
    noticeNew = false;
    got = true;
  }
  portEXIT_CRITICAL(&mux);
  return got;
}

void web::setCredentials(const char* ssid, const char* pass) {
  Creds c;
  memset(&c, 0, sizeof c);
  strlcpy(c.ssid, ssid, sizeof c.ssid);
  strlcpy(c.pass, pass, sizeof c.pass);
  File f = LittleFS.open("/wifi.bin", "w");
  if (!f) return;
  f.write((const uint8_t*)&c, sizeof c);
  f.close();
}

void web::info(Info& o) {
  memset(&o, 0, sizeof o);
  portENTER_CRITICAL(&mux);
  o.on = ready;
  o.hotspot = hotspot;
  strlcpy(o.ssid, hotspot ? "ChordBox" : creds.ssid, sizeof o.ssid);
  strlcpy(o.pass, hotspot ? HOTSPOT_PASSWORD : "", sizeof o.pass);
  strlcpy(o.url, urlBuf, sizeof o.url);
  portEXIT_CRITICAL(&mux);
  o.phones = phones;
}

void web::printInfo(Print& p) {
  p.printf("wifi: %s  %s  network \"%s\"  phones %u\n", hotspot ? "hotspot ChordBox (password " HOTSPOT_PASSWORD ")" : "home network",
           statusBuf, creds.ssid, (unsigned)ws.count());
  p.println("  open http://chordbox.local (or the address above) on the phone");
  p.println("  wifi NETWORK PASSWORD   saves a network; ChordBox joins it after a restart");
}

#else  // Pico and host tests: no Wi-Fi

void web::begin() {}
const char* web::status() { return "NONE"; }
bool web::takeNotice(char*, size_t) { return false; }
void web::setCredentials(const char*, const char*) {}
void web::printInfo(Print& p) { p.println("wifi: this board has none"); }
bool g_fakeWifi = false;                        // host renders: show a joined network
void web::info(Info& o) {
  memset(&o, 0, sizeof o);
  if (!g_fakeWifi) return;
  o.on = true;
  strlcpy(o.ssid, "HomeWiFi", sizeof o.ssid);
  strlcpy(o.url, "http://192.168.1.50", sizeof o.url);
  o.phones = 1;
}

#endif
