// harm_melody.cpp — see harm_melody.h.
//
// Pitches are sampled exactly (forward filtering, backward sampling) over the
// scale notes in range, one phrase (four bars) at a time. Hard rules are
// zero-weight transitions or forbidden states, so they always hold:
//   on-beat notes and notes an 8th or longer are chord notes (the 9th too at Spice 5+)
//   moves of 0..4 scale steps, weighted 2 8 4 2 1; a leap over a third turns back by a step
//   a semitone above a chord note only as a passing 16th; no held "avoid" notes
//   bar starts lean to the 3rd or 7th; the phrase ends on root, 3rd or 5th
// Form A A' A B: later A bars are pulled towards bar 1's motif, moved to the new chord.
#include "harm_melody.h"
#include "bigmem.h"
#include <math.h>
#include <string.h>

namespace hm {

using namespace ht;

static inline int mod12(int x) { return ((x % 12) + 12) % 12; }

namespace {
struct Rng {
  uint32_t s;
  explicit Rng(uint32_t seed) : s(seed * 747796405u + 2891336453u) {
    if (!s) s = 1;
    for (int i = 0; i < 4; i++) next();
  }
  uint32_t next() {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
  }
  float f() { return (next() >> 8) * (1.0f / 16777216.0f); }
  int range(int n) { return n > 0 ? (int)(next() % (uint32_t)n) : 0; }
};
}  // namespace

const char* voiceWord(uint8_t v) {
  static const char* const W[V_COUNT] = {"lead hook", "flute", "house piano", "arp"};
  return v < V_COUNT ? W[v] : "?";
}

uint16_t scaleMask(const MCtx& x) { return x.penta ? pentaMask(x.home, x.dark) : collMask(x.coll); }

void rangeOf(const MCtx& x, int& lo, int& hi) {
  static const int W[5] = {12, 15, 19, 24, 29};
  int w = W[x.range < 0 ? 0 : x.range > 4 ? 4 : x.range];
  int center = 66 + mod12(x.home - 6);             // around F#4..F5, following home
  lo = center - w / 2;
  hi = lo + w;
}

void safePads(const MCtx& x, uint8_t pitch[12]) {
  uint16_t m = scaleMask(x);
  int p = 60 + x.home;
  if (p > 65) p -= 12;
  for (int i = 0; i < 12; p++)
    if (m & (1 << mod12(p))) pitch[i++] = (uint8_t)p;
}

// ------------------------------------------------------------ rhythm
static uint16_t cellMask(int idx) { return rhythmMask(CELLS[idx].steps); }

static uint16_t thin(uint16_t m, int density, Rng& rng) {
  float keep = 0.4f + density * 0.085f;
  uint16_t out = 0;
  bool first = true;
  for (int s = 0; s < 16; s++)
    if (m & (1 << s)) {
      if (first || rng.f() < keep) out |= 1 << s;
      first = false;
    }
  if (density >= 6) {                              // busier: one more offbeat 16th
    for (int t = 0; t < 4; t++) {
      int s = 1 + 2 * rng.range(8);
      if (!(out & (1 << s))) {
        out |= 1 << s;
        break;
      }
    }
  }
  return out ? out : m;
}

static void voiceCells(uint8_t v, int kind, Rng& rng, int density, uint16_t& m) {
  switch (v) {
    case V_LEAD: {
      static const int C[3] = {1, 2, 4};           // tresillo, hook, push
      m = thin(cellMask(C[rng.range(3)]), density, rng);
      break;
    }
    case V_FLUTE: m = cellMask(kind == 3 ? 7 : 6); break;   // long notes; the pickup into the next phrase
    case V_PIANO: m = thin(cellMask(kind == 1 || kind == 3 ? 9 : rng.f() < 0.5f ? 5 : 8), density, rng); break;
    case V_ARP: m = density < 4 ? 0x5555 : 0xFFFF; break;
    default: m = cellMask(1);
  }
}

// ------------------------------------------------------------- pitch
static int stepW(int d, uint8_t v) {               // movement weights by scale steps
  static const int W[5] = {2, 8, 4, 2, 1};
  static const int F[5] = {1, 10, 3, 1, 0};        // flute: stepwise
  if (d > 4) return 0;
  return v == V_FLUTE ? F[d] : W[d];
}

static uint16_t chordNotes(const BarHarm& b, const MCtx& x) {
  uint16_t t = b.tones;
  if (x.spice >= 5 && b.qual != Q_NONE) t |= 1 << mod12(b.rootPc + 2);
  return t;
}

static bool avoidHeld(int pc, const BarHarm& b, const MCtx& x) {   // the "never sustain" table
  int iv = mod12(pc - b.rootPc);
  switch (b.qual) {
    case Q_BRIGHT:
      if (b.homeChord && iv == 5) return true;                        // bright 7 on I: the 4th
      if (!x.dark && mod12(b.rootPc - x.home) == 4 && iv == 2) return true;
      break;
    case Q_DARK:
      if (b.homeChord && modeOf(x.home, x.coll) == 5 && iv == 8) return true;   // natural minor: the b6
      if (!x.dark && mod12(b.rootPc - x.home) == 4 && iv == 2) return true;     // dark 7 on iii: the 9th
      break;
    case Q_DOM: if (iv == 5) return true; break;                      // dominant: the 4th
    case Q_HANG: if (iv == 3 || iv == 4) return true; break;          // hanging: the 3rd
  }
  return false;
}

struct On {
  uint8_t step, len, bar, kind;
  bool beat, first, last;
  int motif;                                       // pitch to lean towards (0 = none)
};

static const int MAXS = 15, MAXON = 64;

// one phrase: FFBS over (scale note, must-turn-back) states
static void samplePhrase(const MCtx& x, const BarHarm* bars, On* on, int non, const int* scale, int ns, int prevPitch,
                         Rng& rng, uint8_t* outPitch, const uint16_t pool) {
  struct Work { float a[MAXON][MAXS * 3]; };
  static Work* W = nullptr;
  bigOnce(W);
  float(*alpha)[MAXS * 3] = W->a;
  float T = 0.6f + 0.1f * x.spice;
  auto unary = [&](int j, int s) -> float {
    const On& o = on[j];
    const BarHarm& b = bars[o.bar];
    int p = scale[s], pc = mod12(p);
    uint16_t cn = chordNotes(b, x);
    bool isChord = cn & (1 << pc);
    if ((o.beat || o.len >= 2) && !isChord && b.qual != Q_NONE) return 0;
    if (!isChord && (b.tones & (1 << mod12(pc - 1))) && (o.len > 1 || o.beat)) return 0;
    if (o.len >= 4 && avoidHeld(pc, b, x)) return 0;
    if (o.last && b.qual != Q_NONE && !(b.tones & (1 << pc))) return 0;
    float c = 0;
    if (o.first) {
      int iv = mod12(pc - b.rootPc);
      c += (iv == 3 || iv == 4 || iv == 10 || iv == 11) ? -0.8f : 0.4f;
    }
    if (o.motif) c += 0.35f * fabsf((float)(p - o.motif));
    if (pool && !(pool & (1 << pc))) c += 1.5f;
    return expf(-c / T);
  };
  auto trans = [&](int s0, int r0, int s1, int& r1) -> float {
    int d = s1 > s0 ? s1 - s0 : s0 - s1;
    if (r0 == 1 && !(s1 == s0 - 1)) return 0;     // leapt up: step back down
    if (r0 == 2 && !(s1 == s0 + 1)) return 0;     // leapt down: step back up
    int w = stepW(d, x.voice);
    if (!w) return 0;
    int semis = scale[s1] - scale[s0];
    r1 = semis > 4 ? 1 : semis < -4 ? 2 : 0;
    return powf((float)w, 1 / T);
  };
  int S3 = ns * 3;
  // first onset: from the previous phrase's last pitch, if any
  for (int s = 0; s < ns; s++)
    for (int r = 0; r < 3; r++) {
      float a = 0;
      if (r == 0) {
        if (prevPitch > 0) {
          int ps = 0, bd = 99;
          for (int k = 0; k < ns; k++)
            if (abs(scale[k] - prevPitch) < bd) bd = abs(scale[k] - prevPitch), ps = k;
          int r1;
          a = trans(ps, 0, s, r1) > 0 ? unary(0, s) : 0;
        } else {
          a = unary(0, s);
        }
      }
      alpha[0][s * 3 + r] = a;
    }
  for (int j = 1; j < non; j++) {
    float sum = 0;
    for (int k = 0; k < S3; k++) alpha[j][k] = 0;
    for (int s0 = 0; s0 < ns; s0++)
      for (int r0 = 0; r0 < 3; r0++) {
        float a0 = alpha[j - 1][s0 * 3 + r0];
        if (a0 <= 0) continue;
        for (int s1 = 0; s1 < ns; s1++) {
          int r1 = 0;
          float t = trans(s0, r0, s1, r1);
          if (t <= 0) continue;
          alpha[j][s1 * 3 + r1] += a0 * t;
        }
      }
    for (int s1 = 0; s1 < ns; s1++) {
      float u = unary(j, s1);
      for (int r = 0; r < 3; r++) sum += (alpha[j][s1 * 3 + r] *= u);
    }
    if (sum <= 0) {                                // nothing fits the rules: relax the motion
      for (int s1 = 0; s1 < ns; s1++) {
        float u = unary(j, s1);
        alpha[j][s1 * 3] = u;
        alpha[j][s1 * 3 + 1] = alpha[j][s1 * 3 + 2] = 0;
        sum += u;
      }
      if (sum <= 0) {
        for (int s1 = 0; s1 < ns; s1++) alpha[j][s1 * 3] = 1;
        sum = (float)ns;
      }
    }
    for (int k = 0; k < S3; k++) alpha[j][k] /= sum;
  }
  // backward
  int cs = -1, cr = 0;
  for (int j = non - 1; j >= 0; j--) {
    float w[MAXS * 3], sum = 0;
    for (int s0 = 0; s0 < ns; s0++)
      for (int r0 = 0; r0 < 3; r0++) {
        float a = alpha[j][s0 * 3 + r0];
        if (a > 0 && cs >= 0) {
          int r1 = 0;
          float t = trans(s0, r0, cs, r1);
          a = (t > 0 && r1 == cr) ? a * t : 0;
        }
        w[s0 * 3 + r0] = a;
        sum += a;
      }
    if (sum <= 0) {                                // fall back: any state of this onset
      for (int k = 0; k < S3; k++) w[k] = alpha[j][k], sum += w[k];
    }
    float r = rng.f() * sum;
    int pick = 0;
    for (int k = 0; k < S3; k++) {
      r -= w[k];
      pick = k;
      if (r <= 0 && w[k] > 0) break;
    }
    cs = pick / 3;
    cr = pick % 3;
    outPitch[j] = (uint8_t)scale[cs];
  }
}

bool generate(const MCtx& x, const BarHarm* bars, int n, const uint16_t* rhythm, Line& out) {
  out.bars = (uint8_t)(n < 1 ? 1 : n > 16 ? 16 : n);
  out.n = 0;
  Rng rng(x.seed);
  int lo, hi;
  rangeOf(x, lo, hi);
  // the scale states: up to 15 notes in range, centred
  int scale[32], ns = 0;
  uint16_t sm = scaleMask(x);
  for (int b = 0; b < out.bars; b++) sm |= bars[b].tones;   // chord notes always count as safe
  for (int p = lo; p <= hi && ns < 32; p++)
    if (sm & (1 << mod12(p))) scale[ns++] = p;
  if (ns > MAXS) {
    int cut = (ns - MAXS) / 2;
    memmove(scale, scale + cut, MAXS * sizeof(int));
    ns = MAXS;
  }
  if (ns < 2) return false;

  // the rhythm of each bar: A bars share bar 1's; B differs
  uint16_t mask[16];
  uint16_t aMask = 0, bMask = 0;
  voiceCells(x.voice, 0, rng, x.density, aMask);
  voiceCells(x.voice, 3, rng, x.density, bMask);
  uint16_t a1Mask = aMask;
  if (x.voice == V_PIANO) voiceCells(x.voice, 1, rng, x.density, a1Mask);   // call, then response
  for (int b = 0; b < out.bars; b++) {
    int kind = b % 4 == 3 ? 3 : b % 4;
    if (rhythm) mask[b] = rhythm[b] ? rhythm[b] : rhythm[0];
    else mask[b] = kind == 3 ? bMask : kind == 1 ? a1Mask : aMask;
    if (rhythm && kind == 3 && rhythm[b] == rhythm[0] && x.spice >= 2) {   // B: answer the tapped rhythm
      uint16_t m = mask[b];
      mask[b] = (uint16_t)((m & 0x00FF) | ((m >> 2) & 0xFF00));
      if (!mask[b]) mask[b] = m;
    }
  }

  // LEAD: two to four distinct notes, mostly repeats
  uint16_t pool = 0;
  if (x.voice == V_LEAD) {
    int picked = 0;
    for (int s = ns / 2, k = 0; k < ns && picked < 3; k++) {
      int idx = (s + ((k & 1) ? (k + 1) / 2 : -k / 2) + ns) % ns;
      int pc = mod12(scale[idx]);
      if (bars[0].tones & (1 << pc) && !(pool & (1 << pc))) pool |= 1 << pc, picked++;
    }
    pool |= sm & (1 << mod12(scale[ns / 2] + (x.dark ? 3 : 2)));
  }

  // ARP: the shape's notes in order
  if (x.voice == V_ARP) {
    for (int b = 0; b < out.bars && out.n < 128; b++) {
      const BarHarm& bh = bars[b];
      int notes[8], nn = 0;
      for (int i = 0; i < bh.nShape && nn < 8; i++) {
        int p = bh.shape[i];
        while (p < lo) p += 12;
        while (p > hi) p -= 12;
        if (p >= lo) notes[nn++] = p;
      }
      if (!nn)
        for (int p = lo; p <= hi && nn < 8; p++)
          if (bh.tones & (1 << mod12(p))) notes[nn++] = p;
      for (int i = 1; i < nn; i++)
        for (int j = i; j > 0 && notes[j] < notes[j - 1]; j--) {
          int t = notes[j];
          notes[j] = notes[j - 1];
          notes[j - 1] = t;
        }
      int k = 0;
      for (int s = 0; s < 16 && nn && out.n < 128; s++)
        if (mask[b] & (1 << s)) out.note[out.n++] = {(uint8_t)(b * 16 + s), (uint8_t)notes[k++ % nn],
                                                      (uint8_t)(x.density < 4 ? 2 : 1), (uint8_t)(s % 4 == 0 ? 90 : 70)};
    }
    return true;
  }

  // onsets with lengths, phrase by phrase
  static On on[MAXON];
  static uint8_t pitch[MAXON];
  int motifBar0[16];
  int motifN = 0;
  int prevPitch = 0;
  for (int ph = 0; ph < out.bars; ph += 4) {
    int non = 0;
    int phraseEnd = ph + 4 < out.bars ? ph + 4 : out.bars;
    for (int b = ph; b < phraseEnd; b++) {
      int kind = b % 4 == 3 ? 3 : b % 4;
      int idxInBar = 0;
      for (int s = 0; s < 16 && non < MAXON; s++) {
        if (!(mask[b] & (1 << s))) continue;
        int next = 16;
        for (int t = s + 1; t < 16; t++)
          if (mask[b] & (1 << t)) {
            next = t;
            break;
          }
        int len = next - s;
        if (x.voice == V_LEAD && len > 2) len = 2;
        if (x.voice == V_PIANO && len > 2) len = 2;
        if (x.voice == V_FLUTE && next == 16 && (b % 2 == 1)) len = len > 3 ? len - 2 : len;   // breathe
        On o{(uint8_t)s, (uint8_t)(len < 1 ? 1 : len), (uint8_t)b, (uint8_t)kind, s % 4 == 0, idxInBar == 0, false, 0};
        if (kind != 3 && b % 4 != 0 && idxInBar < motifN) {   // A and A': lean towards the motif
          int shift = mod12(bars[b].rootPc - bars[b - b % 4].rootPc);
          if (shift > 6) shift -= 12;
          o.motif = motifBar0[idxInBar] + (kind == 1 ? shift : 0);
        }
        on[non++] = o;
        idxInBar++;
      }
    }
    if (!non) continue;
    on[non - 1].last = true;
    samplePhrase(x, bars, on, non, scale, ns, prevPitch, rng, pitch, pool);
    if (ph == 0) {                                  // remember bar 1 as the motif
      motifN = 0;
      for (int j = 0; j < non && on[j].bar == 0 && motifN < 16; j++) motifBar0[motifN++] = pitch[j];
    }
    for (int j = 0; j < non && out.n < 128; j++) {
      const On& o = on[j];
      uint8_t vel = x.voice == V_PIANO ? (o.beat || o.first ? 100 : 70) : x.voice == V_FLUTE ? 80 : (o.beat ? 96 : 82);
      int step = o.bar * 16 + o.step;
      if (x.voice == V_FLUTE && o.len >= 4 && o.step % 4 != 1 && step > 0 && out.n < 127) {   // a grace note
        int g = -1;
        for (int k = 0; k < ns; k++)
          if (scale[k] == pitch[j] && k + 1 < ns) g = scale[k + 1];
        bool free = out.n == 0 || out.note[out.n - 1].step + out.note[out.n - 1].len <= step - 1;
        if (g > 0 && free && (step - 1) % 4 != 0) out.note[out.n++] = {(uint8_t)(step - 1), (uint8_t)g, 1, 60};
      }
      out.note[out.n++] = {(uint8_t)step, pitch[j], o.len, vel};
      if (x.voice == V_PIANO && out.n < 128) {      // chord notes in octaves
        int d = pitch[j] - 12 >= lo ? pitch[j] - 12 : pitch[j] + 12 <= hi ? pitch[j] + 12 : 0;
        if (d) out.note[out.n++] = {(uint8_t)step, (uint8_t)d, o.len, (uint8_t)(vel - 10)};
      }
    }
    prevPitch = pitch[non - 1];
  }
  return true;
}

int check(const Line& l, const BarHarm* bars, const MCtx& x) {
  int lo, hi;
  rangeOf(x, lo, hi);
  for (int i = 0; i < l.n; i++) {
    const Note& nt = l.note[i];
    if (nt.pitch < lo || nt.pitch > hi) return 1;
    const BarHarm& b = bars[nt.step / 16];
    if (b.qual == Q_NONE) continue;
    uint16_t cn = chordNotes(b, x);
    if (x.voice == V_ARP)
      for (int k = 0; k < b.nShape; k++) cn |= 1 << mod12(b.shape[k]);
    if (nt.step % 4 == 0 && !(cn & (1 << mod12(nt.pitch)))) return 2;
  }
  return 0;
}

}  // namespace hm
