// clock.h — follows the Move's MIDI clock, or runs its own tempo
#pragma once
#include <Arduino.h>

namespace clk {

void begin();
void midiRealtime(uint8_t status, int64_t tUs);   // F8 clock, FA start, FB continue, FC stop
void songPosition(uint16_t sixteenths);           // F2
void poll(int64_t nowUs);                         // internal ticks + due steps

bool external();          // Move clock seen recently
bool running();
float bpm();              // what is playing now (Move's, or internal)
float internalBpm();
void setInternalBpm(float bpm);
void tap(int64_t nowUs);  // tap tempo (internal)
void startInternal();     // runs the bass without the Move (preview)
void stopInternal();
void resync();            // the next 16th becomes bar step 0
void setNudgeUs(int32_t us);   // + = send early (cancel USB latency)
void setFollowMove(bool on);   // CLOCK: AUTO follows the Move's clock, INTERNAL ignores it

uint32_t step();          // 16ths since start
int64_t stepAtUs();       // when that step began
int32_t stepUs();         // current 16th length

}  // namespace clk

// provided by the sequencer
void clockOnStep(uint32_t step, int64_t atUs, int32_t stepUs);
void clockOnTransport(bool running);
