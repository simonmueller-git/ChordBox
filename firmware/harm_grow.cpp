// harm_grow.cpp — see harm_grow.h.
//
// A loop that sounds good is a short loop repeated literally, changed only at
// its phrase ends. The A section the user entered never changes; repeats
// change only at their ends (and, with more Spice, slot 6 of 8 may go Outside);
// a B section (A A B A) is new material. Chord choices at the variation points
// come from a first-order chain sampled exactly (forward filtering, backward
// sampling), conditioned on the fixed slots; the loop is cyclic and slot 1 is
// always fixed, so the chain closes on it.
#include "harm_grow.h"
#include "bigmem.h"
#include <math.h>
#include <string.h>
#include <initializer_list>

namespace hg {

using namespace ht;

static inline int mod12(int x) { return ((x % 12) + 12) % 12; }
static int popc(uint16_t m) {
  int n = 0;
  for (; m; m &= m - 1) n++;
  return n;
}

namespace {
struct Rng {
  uint32_t s;
  explicit Rng(uint32_t seed) : s(seed * 2654435761u + 0x9E3779B9u) {
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

const int MAXOPT = 24;
struct Opt {
  Slot s;
  float cost;
};

enum Kind : uint8_t { K_A, K_A1, K_A2, K_B };
}  // namespace

const char* opWord(uint8_t op) {
  static const char* const W[] = {"", "way back", "third swap", "split", "outside", "many basses",
                                  "stuck low", "slide", "chromatic", "free"};
  return op < sizeof W / sizeof W[0] ? W[op] : "";
}

bool outside(const Chord& c, const Ctx& x) {
  if (c.planed || c.qual == Q_NONE) return false;
  uint16_t t = chordTones(c, x.home);
  return (t & collMask(x.coll)) != t;
}

float tension(int i, int len) {
  if (len <= 1) return 0;
  float t = (float)(i % len) / (len - 1);
  return t * t;
}

static const Chord& firstOf(const Slot& s) { return s.c; }
static const Chord& lastOf(const Slot& s) { return (s.flags & HF_SPLIT) ? s.c2 : s.c; }

static bool lowClash(const Slot& s, const Ctx& x) {     // rule 4: a chord note a semitone above the low note
  if (s.low < 0) return false;
  int above = mod12(x.home + s.low + 1);
  if (chordTones(s.c, x.home) & (1 << above)) return true;
  if ((s.flags & HF_SPLIT) && (chordTones(s.c2, x.home) & (1 << above))) return true;
  return false;
}

static bool outsideOk(const Chord& a, const Chord& b, const Ctx& x) {   // rule 3
  if (!outside(a, x)) return true;
  if (chordTones(a, x.home) & chordTones(b, x.home)) return true;
  int d = mod12(chordRootPc(b, x.home) - chordRootPc(a, x.home));
  return d == 1 || d == 2 || d == 10 || d == 11 || d == 5 || d == 7;
}

static float transW(const Slot& a, const Slot& b, const Ctx& x, float T) {
  const Chord& from = lastOf(a);
  const Chord& to = firstOf(b);
  if (!outsideOk(from, to, x)) return 0;
  if ((a.flags & HF_SPLIT) && !outsideOk(a.c, a.c2, x)) return 0;
  if (from.planed || to.planed) return powf(0.2f, 1 / T);
  return powf(nextWeight(from, to, x.dark), 1 / T);
}

// ---------------------------------------------------------------- options
static void addOpt(Opt* o, int& n, const Slot& base, Chord c, float cost, uint8_t op, const Ctx& x) {
  if (n >= MAXOPT || c.qual == Q_NONE) return;
  Slot s = base;
  s.c = c;
  s.flags = (uint8_t)((base.flags & ~(HF_SPLIT | HF_LOCK)) | HF_SET);
  s.low = -1;
  s.op = op;
  if (lowClash(s, x)) return;
  for (int i = 0; i < n; i++)
    if (o[i].s.c == s.c && !(o[i].s.flags & HF_SPLIT) && o[i].s.low == s.low) return;
  o[n++] = {s, cost};
}

static void addSplit(Opt* o, int& n, const Slot& base, Chord a, Chord b, float cost, const Ctx& x) {
  if (n >= MAXOPT) return;
  Slot s = base;
  s.c = a;
  s.c2 = b;
  s.shape2 = base.shape;
  s.flags = (uint8_t)((base.flags & ~HF_LOCK) | HF_SPLIT | HF_SET);
  s.low = -1;
  s.op = OP_SPLIT;
  if (lowClash(s, x) || !outsideOk(a, b, x)) return;
  o[n++] = {s, cost};
}

static void wayBack(Opt* o, int& n, const Slot& base, const Ctx& x) {
  if (x.dark) {
    addOpt(o, n, base, {10, Q_BRIGHT, 0}, 0.2f, OP_WAYBACK, x);   // bVII
    addOpt(o, n, base, {7, Q_DARK, 0}, 0.4f, OP_WAYBACK, x);      // v7
    addSplit(o, n, base, {8, Q_BRIGHT, 0}, {10, Q_BRIGHT, 0}, 0.6f, x);   // bVI then bVII
    addOpt(o, n, base, {7, Q_HANG, 0}, 0.8f, OP_WAYBACK, x);      // V9sus
  } else {
    addOpt(o, n, base, {10, Q_BRIGHT, 0}, 0.3f, OP_WAYBACK, x);   // bVII
    addOpt(o, n, base, {7, Q_HANG, 0}, 0.2f, OP_WAYBACK, x);      // V9sus
    addSplit(o, n, base, {5, Q_BRIGHT, 0}, {7, Q_BRIGHT, 0}, 0.6f, x);   // IV then V
    addOpt(o, n, base, {7, Q_DOM, 0}, 0.7f, OP_WAYBACK, x);       // V7
  }
  if (n) o[n - 1].s.op = o[n - 1].s.op ? o[n - 1].s.op : OP_WAYBACK;
}

static void thirdSwap(Opt* o, int& n, const Slot& base, const Ctx& x) {   // i/bIII/bVI, iv/bVI, I/vi/iii, IV/ii
  const Chord& c = base.c;
  if (c.planed || c.qual == Q_NONE) return;
  uint16_t t = chordTones(c, x.home), set = collMask(x.coll);
  for (int d : {3, 4, 8, 9})
    for (uint8_t q : {Q_DARK, Q_BRIGHT}) {
      Chord s{(int8_t)mod12(c.root + d), q, 0};
      uint16_t u = chordTones(s, x.home);
      if ((u & set) != u || popc(u & t) < 2) continue;
      addOpt(o, n, base, s, 0.5f, OP_SWAP, x);
    }
}

static void outsideOpts(Opt* o, int& n, const Slot& base, int i, int len, const Slot* next, const Ctx& x, float cost) {
  if (x.dark) {
    addOpt(o, n, base, {5, Q_BRIGHT, 0}, cost, OP_OUTSIDE, x);                       // IV
    if (next && next->c.root == 0 && !next->c.planed)
      addOpt(o, n, base, {1, Q_BRIGHT, 0}, cost, OP_OUTSIDE, x);                     // bIImaj7 before i
    if (i == len - 1 && len == 16) addOpt(o, n, base, {7, Q_DOM, 0}, cost, OP_OUTSIDE, x);   // V7 at slot 16
  } else {
    addOpt(o, n, base, {5, Q_DARK, 0}, cost, OP_OUTSIDE, x);                         // iv
    addOpt(o, n, base, {8, Q_BRIGHT, 0}, cost, OP_OUTSIDE, x);                       // bVI
    addOpt(o, n, base, {10, Q_BRIGHT, 0}, cost, OP_OUTSIDE, x);                      // bVII
  }
}

static void manyBasses(Opt* o, int& n, const Slot& base, const Ctx& x) {   // hold the shape, move the low note
  if (base.c.planed || base.c.qual == Q_NONE || n >= MAXOPT) return;
  for (int d : {3, 4, 7}) {
    Slot s = base;
    s.flags = (uint8_t)((base.flags & ~(HF_SPLIT | HF_LOCK)) | HF_SET);
    s.low = (int8_t)mod12(base.c.root - d);
    s.op = OP_BASSES;
    if (!(collMask(x.coll) & (1 << mod12(x.home + s.low))) || lowClash(s, x)) continue;
    o[n++] = {s, 0.8f};
    if (n >= MAXOPT) return;
  }
}

static void chromatic(Opt* o, int& n, const Slot& base, const Ctx& x) {    // a chord a chromatic third away
  const Chord& c = base.c;
  if (c.planed || c.qual == Q_NONE) return;
  for (int d : {3, 4, 8, 9}) {
    Chord s{(int8_t)mod12(c.root + d), Q_BRIGHT, 0};
    if (!outside(s, x)) continue;
    addOpt(o, n, base, s, 1.0f, OP_CHROMA, x);
    return;
  }
}

// ------------------------------------------------------------------- plan
static void kinds(int len, int m, uint8_t form, Kind* k) {
  int secs = len / m;
  for (int i = 0; i < len; i++) {
    int sec = i / m;
    Kind kk = K_A;
    if (secs == 2) kk = sec == 0 ? K_A : K_A1;
    else if (secs == 4) {
      if (form == FORM_AABA) kk = sec == 2 ? K_B : sec == 3 ? K_A2 : K_A;
      else kk = sec == 1 ? K_A1 : sec == 3 ? K_A2 : K_A;
    } else if (secs == 1) {
      kk = K_A1;                                   // a 4-slot loop varies only its own ending
    }
    k[i] = kk;
  }
}

static const int EDITS_PER_8[8] = {0, 0, 0, 1, 2, 2, 3, 4};

void generate(const Loop& in, int len, const Ctx& x, Loop& out) {
  out = in;
  if (len != 4 && len != 8 && len != 16) len = 16;
  out.len = (uint8_t)len;
  if (in.len >= len) {                              // shrinking: keep the start
    if (x.spice == 0) return;
  }
  int m = in.len < len ? in.len : (len >= 8 ? len / 2 : len);
  if (m < 1) m = 1;
  if (len % m) m = len;                            // safety: sections must tile
  auto fixedIn = [&](int i) { return i < in.len && (i == 0 || (in.s[i].flags & HF_LOCK)); };
  for (int i = 0; i < len; i++) {
    if (fixedIn(i) || i < m) out.s[i] = in.s[i];
    else out.s[i] = in.s[i % m];
    if (!fixedIn(i)) out.s[i].flags &= ~HF_LOCK;
  }
  if (x.spice <= 0) return;

  Rng rng(x.seed);
  uint8_t form = in.form;
  if (form == FORM_AUTO && len == 16 && m == 4 && x.spice >= 5 && rng.f() < 0.4f) form = FORM_AABA;
  Kind kind[16];
  kinds(len, m, form, kind);
  auto fixedOut = [&](int i) { return fixedIn(i) || i < m || kind[i] == K_A; };

  // ---- chords (Spice 3 and up)
  if (x.spice >= 3) {
    // variation points: phrase ends of A' / A'' (and the middle of an 8-slot A' at Spice 4+),
    // slot 6 of 8 for Outside at Spice 5+, every slot of a B section
    int8_t pt[16];
    int npt = 0;
    bool active[16] = {false};
    for (int i = 0; i < len; i++) {
      if (fixedOut(i) || kind[i] == K_B) continue;
      bool end = i % m == m - 1 || (m == 8 && i % 8 == 3 && x.spice >= 4);
      bool out6 = x.spice >= 5 && i % 8 == 5;
      if (end || out6) pt[npt++] = (int8_t)i;
    }
    int limit = EDITS_PER_8[x.spice < 8 ? x.spice : 7] * (len >= 8 ? len / 8 : 1);
    if (npt) active[pt[npt - 1]] = true;           // the ending always may change
    int used = 1;
    for (int t = 0; t < 16 && used < limit; t++) {
      int i = pt[rng.range(npt)];
      if (!active[i]) active[i] = true, used++;
    }

    struct Work { Opt opt[16][MAXOPT]; float alpha[16][MAXOPT]; };
    static Work* W = nullptr;
    bigOnce(W);
    Opt(*opt)[MAXOPT] = W->opt;
    int no[16];
    Loop best = out;
    bool okFound = false;
    for (int attempt = 0; attempt < 30 && !okFound; attempt++) {
      for (int i = 0; i < len; i++) {
        no[i] = 0;
        const Slot& lit = out.s[i];
        bool free = !fixedOut(i) && kind[i] == K_B;
        bool var = !fixedOut(i) && active[i];
        if (!free && !var) {
          opt[i][no[i]++] = {lit, 0};
          continue;
        }
        if (free) {                                // B section: the vocabulary, steered by the chain
          Chord pads[12];
          int np = chordPads(x.home, x.coll, pads);
          for (int p = 0; p < np; p++)
            if (pads[p].qual != Q_POWER) addOpt(opt[i], no[i], lit, pads[p], outside(pads[p], x) ? 0.6f : 0, OP_FREE, x);
          if (i % m == m - 1) wayBack(opt[i], no[i], lit, x);
          if (!no[i]) opt[i][no[i]++] = {lit, 0};
          continue;
        }
        bool end = i % m == m - 1 || (m == 8 && i % 8 == 3);
        opt[i][no[i]++] = {lit, end ? 1.0f : 0};     // the literal repeat stays possible
        if (end) {
          wayBack(opt[i], no[i], lit, x);
          thirdSwap(opt[i], no[i], lit, x);
          if (i % 8 == 3 || i % 8 == 7) {            // split / hanging: slots 4 and 8 only
            Chord hang = lit.c;
            hang.qual = Q_HANG;
            if (!lit.c.planed && lit.c.qual != Q_NONE) addSplit(opt[i], no[i], lit, hang, lit.c, 0.6f, x);
            Chord nx[8];
            if (!lit.c.planed && nextChords(lit.c, x.dark, nx)) addSplit(opt[i], no[i], lit, lit.c, nx[0], 0.7f, x);
          }
          if (x.spice >= 5) manyBasses(opt[i], no[i], lit, x);
          if (x.spice >= 7 && i >= 8) chromatic(opt[i], no[i], lit, x);
        }
        if (x.spice >= 5 && (i % 8 == 3 || i % 8 == 5 || i % 8 == 7))
          outsideOpts(opt[i], no[i], lit, i, len, i + 1 < len ? &out.s[i + 1] : &out.s[0], x, end ? 0.7f : 1.2f);
      }

      // forward filtering
      float T = 0.35f + 0.15f * x.spice;
      float(*alpha)[MAXOPT] = W->alpha;
      for (int k = 0; k < no[0]; k++) alpha[0][k] = 1;
      for (int i = 1; i < len; i++) {
        float sum = 0;
        for (int k = 0; k < no[i]; k++) {
          float a = 0;
          for (int j = 0; j < no[i - 1]; j++) a += alpha[i - 1][j] * transW(opt[i - 1][j].s, opt[i][k].s, x, T);
          a *= expf(-opt[i][k].cost / T);
          alpha[i][k] = a;
          sum += a;
        }
        if (sum <= 0) {                            // dead end: allow anything listed, uniformly
          for (int k = 0; k < no[i]; k++) alpha[i][k] = 1;
          sum = (float)no[i];
        }
        for (int k = 0; k < no[i]; k++) alpha[i][k] /= sum;
      }
      // backward sampling, closing the cycle on slot 1
      Loop cand = out;
      int pick = -1;
      for (int i = len - 1; i >= 0; i--) {
        const Slot& after = i == len - 1 ? opt[0][0].s : cand.s[i + 1];
        float w[MAXOPT], sum = 0;
        for (int k = 0; k < no[i]; k++) sum += (w[k] = alpha[i][k] * transW(opt[i][k].s, after, x, T));
        if (sum <= 0) {
          for (int k = 0; k < no[i]; k++) w[k] = alpha[i][k];
          sum = 0;
          for (int k = 0; k < no[i]; k++) sum += w[k];
        }
        float r = rng.f() * sum;
        pick = 0;
        for (int k = 0; k < no[i]; k++) {
          r -= w[k];
          if (r <= 0) {
            pick = k;
            break;
          }
          pick = k;
        }
        cand.s[i] = opt[i][pick].s;
      }
      cand.form = form;
      // rules the chain can't see: <= 2 Outside in a row, one Outside per 8 slots
      int run = 0, perEight[2] = {0, 0};
      bool ok = true;
      for (int i = 0; i < len && ok; i++) {
        for (int h = 0; h < ((cand.s[i].flags & HF_SPLIT) ? 2 : 1); h++) {
          const Chord& c = h ? cand.s[i].c2 : cand.s[i].c;
          if (outside(c, x)) {
            run++;
            if (!fixedOut(i) && cand.s[i].op == OP_OUTSIDE) perEight[i / 8 < 2 ? i / 8 : 1]++;
          } else {
            run = 0;
          }
          if (run > 2) ok = false;
        }
      }
      if (perEight[0] > 1 || perEight[1] > 1) ok = false;
      if (ok && violates(cand, in, x) == 0) {
        best = cand;
        okFound = true;
      }
    }
    out = best;
  }

  // ---- phrase operators: stuck low note (Spice 5+), slide shape (Spice 7)
  if (x.spice >= 5 && rng.f() < 0.3f) {
    for (int tries = 0; tries < 6; tries++) {
      int start = m + rng.range(len - m), run = 2 + rng.range(3);
      int8_t low = rng.f() < 0.5f ? 0 : 7;
      bool ok = start + run <= len;
      for (int i = start; ok && i < start + run; i++) {
        Slot s = out.s[i];
        s.low = low;
        ok = !fixedOut(i) && kind[i] != K_A && !s.c.planed && !lowClash(s, x);
      }
      if (!ok) continue;
      for (int i = start; i < start + run; i++) out.s[i].low = low, out.s[i].op = OP_STUCK;
      break;
    }
  }
  if (x.spice >= 7 && rng.f() < 0.4f && out.s[0].c.qual != Q_NONE) {
    int from = -1, to = -1;
    for (int i = 0; i < len; i++)
      if (kind[i] == K_B && !fixedOut(i)) {
        if (from < 0) from = i;
        to = i;
      }
    if (from < 0 && len >= 8) from = len - 2, to = len - 1;
    static const int8_t PENTA[5] = {0, 3, 5, 7, 10};
    for (int i = from; from >= 0 && i <= to; i++) {
      if (fixedOut(i)) continue;
      int want = out.s[i].c.planed ? mod12(out.s[i].c.root - x.home) : out.s[i].c.root, bestR = 0, bd = 99;
      for (int8_t p : PENTA) {
        int d = abs(mod12(p - want + 6) - 6);
        if (d < bd) bd = d, bestR = p;
      }
      Slot s = out.s[i];
      s.c = {(int8_t)mod12(x.home + bestR), out.s[0].c.qual, 1};
      s.shape = out.s[0].shape == SH_THIN ? SH_BLOCK : out.s[0].shape;
      s.flags &= ~HF_SPLIT;
      s.low = -1;
      s.op = OP_SLIDE;
      out.s[i] = s;
    }
    if (violates(out, in, x)) {                   // never at the cost of a rule: undo the slide
      for (int i = from; from >= 0 && i <= to; i++)
        if (!fixedOut(i) && out.s[i].op == OP_SLIDE) out.s[i] = in.s[i % m];
    }
  }

  // ---- shape: top-note hold, thin A / full A', a flip at the ends; Early rhythm
  {
    int pick = rng.range(3);
    int iv = pick == 0 ? 7 : pick == 1 ? (x.dark ? 3 : 4) : 14;   // 5th, (lowered) 3rd, 9th of home
    int t = 60 + mod12(x.home + iv);
    while (t < 64) t += 12;
    while (t > 72) t -= 12;
    out.top = (uint8_t)t;
  }
  for (int i = m; i < len; i++) {
    if (fixedIn(i)) continue;
    Slot& s = out.s[i];
    if (s.c.planed) continue;
    if (kind[i] != K_A && s.shape == SH_THIN) s.shape = SH_FULL;
    if (x.spice >= 2 && tension(i, len) > 0.8f && (i % m == m - 1)) s.shape = SH_FLIP;
  }
  // Early on at most half of the changes, at the same places in every section
  int changes[16], nch = 0;
  for (int i = 1; i < m; i++)
    if (!(out.s[i].c == out.s[i - 1].c)) changes[nch++] = i;
  int pushes = x.spice >= 2 ? nch / 2 : (nch ? 1 : 0);
  for (int k = 0; k < pushes; k++) {
    int j = rng.range(nch);
    int pos = changes[j];
    for (int i = pos; i < len; i += m)
      if (!fixedIn(i) && i >= m) out.s[i].flags |= HF_PUSH;
    changes[j] = changes[--nch];
  }
}

float score(const Loop& l, const Ctx& x) {
  float sc = 0;
  uint8_t prev[8], cur[8];
  int np = 0;
  int firstTop = -1;
  for (int i = 0; i <= l.len; i++) {
    const Slot& s = l.s[i % l.len];
    if (s.c.qual == Q_NONE) continue;
    int8_t off[6];
    int no = shapeOffsets(s.c.qual, s.shape, s.c.root == 5, off);
    int nc = place(off, no, chordRootPc(s.c, x.home), prev, np, l.top, cur);
    if (np && nc) {
      if (abs((int)cur[nc - 1] - (int)prev[np - 1]) <= 2) sc += 1;
      if (firstTop < 0) firstTop = cur[nc - 1];
    }
    memcpy(prev, cur, nc);
    np = nc;
    if (i < l.len) {
      const Slot& n = l.s[(i + 1) % l.len];
      sc += popc(chordTones(lastOf(s), x.home) & chordTones(firstOf(n), x.home)) * 0.5f;
      sc += nextWeight(lastOf(s), firstOf(n), x.dark) * 4;
    }
  }
  return sc;
}

int candidates(const Loop& in, int len, const Ctx& x, int page, Loop* out, int n) {
  struct Pool { Loop l[20]; };
  static Pool* PL = nullptr;
  bigOnce(PL);
  Loop* pool = PL->l;
  static float sc[20];
  int np = 0;
  int tries = x.spice <= 0 ? 1 : 20;
  for (int t = 0; t < tries; t++) {
    Ctx c = x;
    c.seed = x.seed * 7919u + (uint32_t)t * 104729u + 1;
    Loop l;
    generate(in, len, c, l);
    bool dup = false;
    for (int k = 0; k < np && !dup; k++) dup = pool[k] == l;
    if (dup) continue;
    pool[np] = l;
    sc[np] = score(l, x);
    np++;
  }
  for (int i = 0; i < np; i++)                    // best first
    for (int j = i + 1; j < np; j++)
      if (sc[j] > sc[i]) {
        float t = sc[i];
        sc[i] = sc[j];
        sc[j] = t;
        Loop tl = pool[i];
        pool[i] = pool[j];
        pool[j] = tl;
      }
  int k = 0;
  for (int i = page * n; i < np && k < n; i++) out[k++] = pool[i];
  return k;
}

int suggest(const Loop& l, int i, const Ctx& x, Chord out[8]) {
  Chord prev;
  if (i > 0) prev = lastOf(l.s[i - 1]);
  else if (l.len > 1 && (l.s[l.len - 1].flags & HF_SET)) prev = lastOf(l.s[l.len - 1]);
  else prev = {0, (uint8_t)(x.dark ? Q_DARK : Q_BRIGHT), 0};
  if (prev.qual == Q_NONE) prev = {0, (uint8_t)(x.dark ? Q_DARK : Q_BRIGHT), 0};
  Chord nx[8];
  int n = nextChords(prev, x.dark, nx), k = 0;
  for (int j = 0; j < n && k < 7; j++)
    if (outsideOk(prev, nx[j], x)) out[k++] = nx[j];
  bool hasStay = false;
  for (int j = 0; j < k; j++) hasStay |= out[j] == prev;
  if (!hasStay && k < 8 && !prev.planed) out[k++] = prev;
  return k;
}

int violates(const Loop& o, const Loop& in, const Ctx& x) {
  // 5: slot 1 and locked slots bit-exact
  for (int i = 0; i < in.len && i < o.len; i++)
    if ((i == 0 || (in.s[i].flags & HF_LOCK)) && !(o.s[i] == in.s[i])) return 5;
  // 1: no diminished chords (none exist in the vocabulary; NONE only where it was entered)
  for (int i = 0; i < o.len; i++)
    if (o.s[i].c.qual > Q_NONE) return 1;
  // 2 and 3: Outside chords, cyclically
  int n = 0;
  Chord seq[32];
  for (int i = 0; i < o.len; i++) {
    if (o.s[i].c.qual == Q_NONE) continue;
    seq[n++] = o.s[i].c;
    if (o.s[i].flags & HF_SPLIT) seq[n++] = o.s[i].c2;
  }
  int run = 0;
  for (int i = 0; i < n; i++) {
    run = outside(seq[i], x) ? run + 1 : 0;
    if (run > 2) return 2;
    if (!outsideOk(seq[i], seq[(i + 1) % n], x)) return 3;
  }
  // 4: no chord note a semitone above a low note
  for (int i = 0; i < o.len; i++)
    if (lowClash(o.s[i], x) && !(i < in.len && o.s[i] == in.s[i])) return 4;
  // 6: at Spice 2 or lower, chords change only on bar lines
  if (x.spice <= 2)
    for (int i = 0; i < o.len; i++)
      if ((o.s[i].flags & HF_SPLIT) && !(i < in.len && (in.s[i].flags & HF_SPLIT))) return 6;
  return 0;
}

void skeleton(int idx, const Ctx& x, Loop& out) {
  out = Loop();
  if (idx < 0 || idx >= N_SKELETONS) return;
  const Skeleton& sk = SKELETONS[idx];
  int shift = sk.dark == x.dark ? 0 : (sk.dark ? 9 : 3);   // relative major / minor
  out.len = sk.len;
  char buf[200];
  strncpy(buf, sk.bars, sizeof buf - 1);
  buf[sizeof buf - 1] = 0;
  int i = 0;
  for (char* tok = buf; *tok && i < 16;) {
    while (*tok == ' ') tok++;
    char* end = tok;
    while (*end && *end != ' ') end++;
    char save = *end;
    *end = 0;
    Slot s;
    s.flags = HF_SET;
    char* bar = strchr(tok, '|');
    char* slash = strchr(tok, '/');
    if (slash) *slash = 0;
    if (bar) *bar = 0;
    uint8_t sh;
    Chord c;
    if (parseRoman(tok, c, sh)) {
      c.root = (int8_t)mod12(c.root + shift);
      if (c.planed) c.root = (int8_t)mod12(x.home + c.root);
      s.c = c;
      s.shape = sh;
      if (bar && parseRoman(bar + 1, c, sh)) {
        c.root = (int8_t)mod12(c.root + shift);
        if (c.planed) c.root = (int8_t)mod12(x.home + c.root);
        s.c2 = c;
        s.shape2 = sh;
        s.flags |= HF_SPLIT;
      }
      if (slash) s.low = (int8_t)shift;            // "/1": the low note stays on home
      out.s[i++] = s;
    }
    *end = save;
    tok = end;
  }
  out.len = (uint8_t)(i >= 16 ? 16 : i >= 8 ? 8 : 4);
}

}  // namespace hg
