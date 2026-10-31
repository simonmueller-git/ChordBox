// console.cpp — line commands on USB serial (and the COM port on the ESP32).
// Drives the whole instrument without keypads or a Move: pads, the encoder,
// every setting, and a simulated Move clock. `shot` dumps the framebuffer.

#include <Arduino.h>
#include "app.h"
#include "bass.h"
#include "engine.h"
#include "harm.h"
#include "input.h"
#include "platform.h"
#include "types.h"
#include "ui.h"
#include "web.h"

namespace console {

static String lineUsb, lineUart;
static Stream* out = &Serial;

struct Pending { uint32_t due; Event e; };
static Pending pend[8];
static int nPend = 0;

static void post(EvType t, uint8_t a = 0, int16_t b = 0) {
  Event e{t, a, b, 0};
  postEvent(e);
}

static void later(uint32_t ms, EvType t, uint8_t a) {
  if (nPend < 8) pend[nPend++] = {millis() + ms, {t, a, 0, 0}};
}

static void run(String cmd) {
  cmd.trim();
  if (!cmd.length()) return;
  int sp = cmd.indexOf(' ');
  String verb = sp < 0 ? cmd : cmd.substring(0, sp);
  String rest = sp < 0 ? "" : cmd.substring(sp + 1);
  rest.trim();
  int a = rest.toInt();
  int sp2 = rest.indexOf(' ');
  String first = sp2 < 0 ? rest : rest.substring(0, sp2);
  String arg2 = sp2 < 0 ? "" : rest.substring(sp2 + 1);
  int b = arg2.toInt();

  if (verb == "help" || verb == "?") app::printHelp(*out);
  else if (verb == "state") engine::printState(*out);
  else if (verb == "bass") bass::printState(*out);
  else if (verb == "pattern") bass::printPattern(*out);
  else if (verb == "harm") harm::printState(*out);
  else if (verb == "favs") engine::printFavs(*out);
  else if (verb == "mem") {
#if defined(ARDUINO_ARCH_ESP32)
    out->printf("heap %u  min %u  psram %u\n", ESP.getFreeHeap(), ESP.getMinFreeHeap(), ESP.getFreePsram());
#elif defined(ARDUINO_ARCH_RP2040)
    out->printf("heap free %d  used %d  total %d\n", rp2040.getFreeHeap(), rp2040.getUsedHeap(), rp2040.getTotalHeap());
#endif
  } else if (verb == "perf") ui::perf(*out);
  else if (verb == "reboot") {
#if defined(ARDUINO_ARCH_ESP32)
    ESP.restart();
#elif defined(ARDUINO_ARCH_RP2040)
    rp2040.reboot();
#endif
  } else if (verb == "pad" && a >= 1 && a <= 32) post(EV_PAD_DOWN, a - 1);
  else if (verb == "up" && a >= 1 && a <= 32) post(EV_PAD_UP, a - 1);
  else if (verb == "tap" && a >= 1 && a <= 32) {
    post(EV_PAD_DOWN, a - 1);
    later(b ? b : 200, EV_PAD_UP, a - 1);
  } else if (verb == "turn") post(EV_TURN, 0, a ? a : 1);
  else if (verb == "push") post(EV_PUSH_TURN, 0, a ? a : 1);
  else if (verb == "click") post(EV_CLICK);
  else if (verb == "hold") post(EV_HOLD);
  else if (verb == "release") post(EV_RELEASE);
  else if (verb == "set" || verb == "bset") {
    int p = verb == "set" ? engine::paramIndex(first) : bass::paramIndex(first);
    if (p < 0) out->printf("%s: unknown setting %s (help)\n", verb.c_str(), first.c_str());
    else post(verb == "set" ? EV_SET : EV_BSET, p, b);
  } else if (verb == "act") {
    int id = app::actionIndex(first);             // act NAME [N]
    if (id < 0) out->println("act: latch alloff more star gen mutate favs bstar bmutate edit play resync panic calib mode N newsong");
    else post(EV_ACTION, id, b);
  } else if (verb == "clock") {
    if (rest == "start") simTransport(0xFA);
    else if (rest == "stop") simTransport(0xFC);
    else if (rest == "cont") simTransport(0xFB);
    else simClock(rest.toFloat());
    out->printf("sim clock %s\n", rest.c_str());
  } else if (verb == "shot") ui::shot(*out);
  else if (verb == "lcd") ui::lcdCmd(rest, *out);
  else if (verb == "input") {
    if (first == "knob2") {                      // input knob2 auto|on|off
      input::setKnob2Mode(arg2 == "on" ? 1 : arg2 == "off" ? 2 : 0);
      input::saveSettings();
      out->println("second knob setting saved; it applies after a restart (reboot)");
    } else {
      input::debug(*out);
    }
  }
  else if (verb == "turn2") post(EV_TURN2, 0, a ? a : 1);
  else if (verb == "push2") post(EV_PUSH_TURN2, 0, a ? a : 1);
  else if (verb == "click2") post(EV_CLICK2);
  else if (verb == "btn" && a >= 1 && a <= 4) {
    post(EV_BTN, a - 1, 1);
    later(b > 0 ? b : 150, EV_BTN, a - 1);
  }
  else if (verb == "oled") ui::oledCmd(rest, *out);
  else if (verb == "oledshot") ui::oledShot(*out);
  else if (verb == "wifi") {
    if (!rest.length()) {
      web::printInfo(*out);
    } else {                                     // wifi NETWORK PASSWORD
      int sp3 = rest.lastIndexOf(' ');
      String ssid = sp3 < 0 ? rest : rest.substring(0, sp3), pass = sp3 < 0 ? "" : rest.substring(sp3 + 1);
      web::setCredentials(ssid.c_str(), pass.c_str());
      out->printf("wifi: saved \"%s\"; ChordBox joins it after a restart (reboot)\n", ssid.c_str());
    }
  }
  else out->printf("? %s  (help)\n", cmd.c_str());
}

static void feed(Stream& s, String& line) {
  while (s.available()) {
    char c = s.read();
    if (c == '\r') continue;
    if (c == '\n') {
      out = &s;
      run(line);
      line = "";
    } else if (line.length() < 120) {
      line += c;
    }
  }
}

void poll() {
  if (Serial) feed(Serial, lineUsb);
#if defined(ARDUINO_ARCH_ESP32)
  feed(Serial0, lineUart);
#endif
  uint32_t now = millis();
  for (int i = 0; i < nPend;) {
    if ((int32_t)(now - pend[i].due) >= 0) {
      postEvent(pend[i].e);
      pend[i] = pend[--nPend];
    } else {
      i++;
    }
  }
}

}  // namespace console
