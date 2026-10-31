// Host test: drive the real chord engine and print what it plays, one line
// per pad press, for comparison with the Pico's code.py (tests/host/ref.py).
#include "harness.h"
#include <algorithm>

int main() {
  bootAll();
  const int banks[] = {1, 2, 11, 13, 18};     // Minor, Modal, Mysterious, Dark, House
  const int roots[] = {5, 9};                 // F, A
  for (int voic = 0; voic < 4; voic++)
    for (int root : roots)
      for (int b : banks) {
        engine::paramSet(engine::P_ROOT, root);
        engine::paramSet(engine::P_VOIC, voic);
        engine::paramSet(engine::P_DICE, 0);
        engine::paramSet(engine::P_VLD, 1);
        engine::allOff();                     // also clears the voice-leading memory
        engine::paramSet(engine::P_BANK, b);  // page 0
        ChordView v;
        engine::chordView(v);
        int pages = v.pages;
        for (int pg = 0; pg < pages; pg++) {
          for (int pad = 0; pad < 16; pad++) {
            g_midi.clear();
            engine::padDown(pad);
            std::vector<int> chord;
            for (auto& s : g_midi)
              if ((s.st & 0xF0) == 0x90 && s.d2) chord.push_back(s.d1);
            engine::padUp(pad);
            if (chord.empty()) continue;
            engine::chordView(v);
            std::sort(chord.begin(), chord.end());
            printf("v%d r%d bank%d row%d col%d |", voic, root, b, pg * 4 + pad / 4, pad % 4);
            for (int n : chord) printf(" %d", n);
            printf(" | %s\n", v.sym);
          }
          engine::pageMove(1);
        }
      }
  return 0;
}
