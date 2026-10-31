// clock.cpp — the bass sequencer's time base
//
// External: 24 clocks per quarter from the Move. The tick period is smoothed
// (outliers ignored), and each 16th boundary is *predicted* one tick ahead, so
// a step can be fired a few ms early (NUDGE) to cancel the USB round trip, or
// late to push behind the beat. The first step after START fires on the first
// clock, as MIDI defines.
// Internal: the same tick path, driven by the wall clock at the set tempo.
// Chords never touch this: only the bass runs on the clock.

#include "clock.h"
#include "config.h"

namespace {

bool extSeen = false, runningExt = false, runningInt = false;
int64_t lastTickUs = 0;
double periodUs = 20161.0;          // 124 BPM
int32_t tickIdx = -1;               // clocks since start (step = tickIdx / 6)
int64_t firedStepFor = -1;          // last step already scheduled/fired
int32_t nudgeUs = 0;
bool follow = true;                 // CLOCK = AUTO

float intBpm = 124.0f;
int64_t nextIntTickUs = 0;

struct Pending { bool on; uint32_t step; int64_t at; } pend = {false, 0, 0};

uint32_t curStep = 0;
int64_t curStepAt = 0;

int64_t taps[4];
int nTaps = 0;
bool resyncPending = false;
uint32_t stepOffset = 0;            // resync shifts where bar step 0 falls

double intPeriodUs() { return 60e6 / (intBpm * 24.0); }

void schedule(uint32_t step, int64_t at) {
  if ((int64_t)step <= firedStepFor) return;
  pend = {true, step, at};
  firedStepFor = step;
}

void fire(uint32_t step, int64_t at) {
  if (resyncPending) {
    stepOffset = step;
    resyncPending = false;
  }
  curStep = step - stepOffset;
  curStepAt = at;
  clockOnStep(curStep, at, (int32_t)(periodUs * 6));
}

// one clock tick at time t (external or internal)
void onTick(int64_t t) {
  tickIdx++;
  bool run = runningExt || runningInt;
  if (!run) return;
  if (tickIdx % 6 == 0) {                         // on the boundary itself
    uint32_t s = tickIdx / 6;
    if (tickIdx == 0 || nudgeUs == 0) schedule(s, t);
    else if (nudgeUs < 0) schedule(s, t - nudgeUs);
  } else if ((tickIdx + 1) % 6 == 0 && nudgeUs > 0) {   // one tick before: predict
    uint32_t s = (tickIdx + 1) / 6;
    int64_t predicted = t + (int64_t)periodUs;
    int64_t at = predicted - nudgeUs;
    schedule(s, at < t ? t : at);
  }
}

void transport(bool run) {
  clockOnTransport(run);
}

}  // namespace

void clk::begin() { nextIntTickUs = 0; }

void clk::setFollowMove(bool on) {
  follow = on;
  if (on) return;
  extSeen = false;
  if (runningExt) {
    runningExt = false;
    pend.on = false;
    transport(false);
  }
}

void clk::midiRealtime(uint8_t st, int64_t t) {
  if (!follow) return;
  switch (st) {
    case 0xF8: {
      if (extSeen && lastTickUs) {
        double dt = (double)(t - lastTickUs);
        if (dt > periodUs * 0.3 && dt < periodUs * 3.0) periodUs += (dt - periodUs) * 0.08;
        else if (dt > 2000 && dt < 120000) periodUs = dt;   // tempo jump: take it
      }
      lastTickUs = t;
      if (!extSeen) {
        extSeen = true;
        runningInt = false;                        // the Move is the boss now
      }
      onTick(t);
      break;
    }
    case 0xFA:                                     // start: next clock is step 0
      tickIdx = -1;
      firedStepFor = -1;
      stepOffset = 0;
      runningExt = true;
      runningInt = false;
      pend.on = false;
      transport(true);
      break;
    case 0xFB:                                     // continue
      runningExt = true;
      transport(true);
      break;
    case 0xFC:
      runningExt = false;
      pend.on = false;
      transport(false);
      break;
  }
}

void clk::songPosition(uint16_t sixteenths) {
  if (!follow) return;
  tickIdx = (int32_t)sixteenths * 6 - 1;
  firedStepFor = (int64_t)sixteenths - 1;
}

void clk::poll(int64_t now) {
  if (extSeen && now - lastTickUs > (int64_t)EXT_CLOCK_TIMEOUT_MS * 1000) {
    extSeen = false;
    if (runningExt) {
      runningExt = false;
      transport(false);
    }
  }
  if (runningInt && !extSeen) {
    if (!nextIntTickUs) nextIntTickUs = now;
    periodUs = intPeriodUs();
    while (now >= nextIntTickUs) {
      onTick(nextIntTickUs);
      nextIntTickUs += (int64_t)periodUs;
    }
  }
  if (pend.on && now >= pend.at) {
    pend.on = false;
    fire(pend.step, pend.at);
  }
}

bool clk::external() { return extSeen; }
bool clk::running() { return runningExt || runningInt; }
float clk::bpm() { return (float)(60e6 / (periodUs * 24.0)); }
float clk::internalBpm() { return intBpm; }
void clk::setInternalBpm(float b) { intBpm = constrain(b, 40.0f, 240.0f); }

void clk::tap(int64_t now) {
  if (nTaps && now - taps[nTaps - 1] > 2000000) nTaps = 0;
  if (nTaps == 4) {
    memmove(taps, taps + 1, sizeof(int64_t) * 3);
    nTaps = 3;
  }
  taps[nTaps++] = now;
  if (nTaps >= 2) {
    double avg = (double)(taps[nTaps - 1] - taps[0]) / (nTaps - 1);
    setInternalBpm((float)(60e6 / avg));
  }
}

void clk::startInternal() {
  if (extSeen) {                                   // clock running without START:
    runningExt = true;                             // follow it from the next beat
    tickIdx = -1;
    firedStepFor = -1;
    stepOffset = 0;
    transport(true);
    return;
  }
  runningInt = true;
  tickIdx = -1;
  firedStepFor = -1;
  stepOffset = 0;
  nextIntTickUs = 0;
  transport(true);
}

void clk::stopInternal() {
  bool was = running();
  runningInt = false;
  if (extSeen) runningExt = false;
  pend.on = false;
  if (was) transport(false);
}

void clk::resync() { resyncPending = true; }
void clk::setNudgeUs(int32_t us) { nudgeUs = constrain(us, -20000, 20000); }
uint32_t clk::step() { return curStep; }
int64_t clk::stepAtUs() { return curStepAt; }
int32_t clk::stepUs() { return (int32_t)(periodUs * 6); }
