// harm_listen.cpp — see harm_listen.h. The spec's weights are starting values;
// two were tuned against its section 8 tests and are marked "tuned".
#include "harm_listen.h"
#include "bigmem.h"
#include <math.h>
#include <string.h>
#include <initializer_list>

namespace hl {

static inline int mod12(int x) { return ((x % 12) + 12) % 12; }
static const int MAXB = 64;                          // bars analysed at most

static float metricW(int step) {                     // 4 downbeat, 3 beat 3, 2 other beats, 1 offbeats
  step &= 15;
  return step == 0 ? 4 : step == 8 ? 3 : (step == 4 || step == 12) ? 2 : 1;
}

static float cosSim(const float* a, const float* b) {
  float ab = 0, aa = 0, bb = 0;
  for (int i = 0; i < 12; i++) ab += a[i] * b[i], aa += a[i] * a[i], bb += b[i] * b[i];
  return aa > 0 && bb > 0 ? ab / sqrtf(aa * bb) : (aa == 0 && bb == 0 ? 1 : 0);
}

namespace {
struct Cand {
  int8_t root;
  uint8_t qual;                                      // Q_BRIGHT, Q_DARK, Q_POWER (= no third), Q_NONE
  float score;
};

uint16_t candTones(const Cand& c) {
  if (c.qual == ht::Q_NONE) return 0;
  int r = c.root;
  uint16_t t = (1 << r) | (1 << mod12(r + 7));
  if (c.qual == ht::Q_BRIGHT) t |= 1 << mod12(r + 4);
  if (c.qual == ht::Q_DARK) t |= 1 << mod12(r + 3);
  return t;
}
}  // namespace

bool analyze(const Ev* ev, int n, int bassCh, int forceColl, float msPer16th, Result& r) {
  r = Result();
  memset(r.listenL, 0, sizeof r.listenL);
  r.home.clear();
  memset(r.kept, 0, sizeof r.kept);
  if (n <= 0) return false;

  // ---- cleaning: short or quiet notes out; bass picked; overtone ghosts at a quarter
  struct Work {
    uint8_t ok[512], isBass[512];
    float wt[512];
    float u[MAXB][12], up[MAXB][12], b[MAXB][12];
  };
  static Work* W = nullptr;
  bigOnce(W);
  uint8_t* ok = W->ok;
  uint8_t* isBass = W->isBass;
  float* wt = W->wt;
  if (n > 512) n = 512;
  float ghostWin = 30.0f / msPer16th, minDur = 60.0f / msPer16th;
  float t0 = 1e30f, t1 = -1e30f;
  for (int i = 0; i < n; i++) {
    ok[i] = ev[i].dur >= minDur && ev[i].vel >= 20;
    wt[i] = 1;
    if (!ok[i]) continue;
    t0 = fminf(t0, ev[i].on);
    t1 = fmaxf(t1, ev[i].on + ev[i].dur);
  }
  if (t0 > t1) return false;
  for (int i = 0; i < n; i++) {
    isBass[i] = 0;
    if (!ok[i]) continue;
    if (bassCh >= 0) {
      isBass[i] = ev[i].ch == bassCh;
    } else if (ev[i].pitch < 55) {                   // the lowest sustained note below 55
      float mid = ev[i].on + ev[i].dur / 2;
      bool lowest = true;
      for (int j = 0; j < n && lowest; j++)
        if (j != i && ok[j] && ev[j].pitch < ev[i].pitch && ev[j].on <= mid && ev[j].on + ev[j].dur >= mid)
          lowest = false;
      isBass[i] = lowest;
    }
  }
  for (int i = 0; i < n; i++) {
    if (!ok[i] || isBass[i]) continue;
    for (int j = 0; j < n; j++) {
      if (j == i || !ok[j] || ev[j].vel <= ev[i].vel) continue;
      int d = ev[i].pitch - ev[j].pitch;
      if ((d == 12 || d == 19 || d == 28) && fabsf(ev[i].on - ev[j].on) <= ghostWin) {
        wt[i] = 0.25f;
        break;
      }
    }
  }

  // ---- per bar: upper (metric-weighted, and plain) and bass durations per pitch class
  int firstBar = (int)floorf(t0 / 16), lastBar = (int)floorf((t1 - 0.01f) / 16);
  int bars = lastBar - firstBar + 1;
  if (bars > MAXB) bars = MAXB;
  r.bars = bars;
  r.firstBar = firstBar;
  float(*u)[12] = W->u;
  float(*up)[12] = W->up;
  float(*b)[12] = W->b;
  memset(W->u, 0, sizeof W->u);
  memset(W->up, 0, sizeof W->up);
  memset(W->b, 0, sizeof W->b);
  for (int i = 0; i < n; i++) {
    if (!ok[i]) continue;
    int pc = ev[i].pitch % 12;
    float s = ev[i].on, e = ev[i].on + ev[i].dur;
    for (int bar = (int)floorf(s / 16); bar * 16 < e; bar++) {
      int bi = bar - firstBar;
      if (bi < 0 || bi >= bars) continue;
      float a = fmaxf(s, bar * 16.0f), z = fminf(e, bar * 16.0f + 16);
      if (z <= a) continue;
      float d = (z - a) * wt[i];
      if (isBass[i]) {
        b[bi][pc] += d;
      } else {
        float mw = a == s ? metricW((int)lroundf(s - bar * 16)) : 2;   // held over the bar line: 2
        u[bi][pc] += d * mw;
        up[bi][pc] += d;
      }
    }
  }

  // ---- loop length: the strongest repeat at 1, 2, 4 or 8 bars
  int L = 1;
  if (bars >= 2) {
    float sim[4] = {-1, -1, -1, -1}, best = -1;
    static const int LS[4] = {1, 2, 4, 8};
    for (int li = 0; li < 4; li++) {
      int l = LS[li];
      if (l * 2 > bars) break;
      float s = 0;
      int cnt = 0;
      for (int i = 0; i + l < bars; i++) {
        float va[12], vb[12];
        for (int pc = 0; pc < 12; pc++) va[pc] = up[i][pc] + b[i][pc], vb[pc] = up[i + l][pc] + b[i + l][pc];
        s += cosSim(va, vb);
        cnt++;
      }
      sim[li] = cnt ? s / cnt : -1;
      best = fmaxf(best, sim[li]);
    }
    for (int li = 0; li < 4; li++)
      if (sim[li] >= 0 && sim[li] >= best - 0.05f) {
        L = LS[li];
        break;
      }
  }
  r.loop = L;

  // ---- fold the loop: sum each loop bar over its repeats, drop rare pitch classes
  float fu[8][12], fup[8][12], fb[8][12];
  memset(fu, 0, sizeof fu);
  memset(fup, 0, sizeof fup);
  memset(fb, 0, sizeof fb);
  int reps[8] = {0};
  int seen[8][12];
  memset(seen, 0, sizeof seen);
  for (int bi = 0; bi < bars; bi++) {
    int lb = (firstBar + bi) % L;
    reps[lb]++;
    for (int pc = 0; pc < 12; pc++) {
      if (up[bi][pc] > 0 || b[bi][pc] > 0) seen[lb][pc]++;
      fu[lb][pc] += u[bi][pc];
      fup[lb][pc] += up[bi][pc];
      fb[lb][pc] += b[bi][pc];
    }
  }
  for (int lb = 0; lb < L; lb++) {
    for (int pc = 0; pc < 12; pc++) {
      if (reps[lb] == 0) continue;
      if (seen[lb][pc] * 2 < reps[lb]) fu[lb][pc] = fup[lb][pc] = fb[lb][pc] = 0;   // in fewer than half
      fu[lb][pc] /= reps[lb];
      fup[lb][pc] /= reps[lb];
      fb[lb][pc] /= reps[lb];
      if (fup[lb][pc] > 0 || fb[lb][pc] > 0) r.kept[lb] |= 1 << pc;
    }
  }

  // ---- note set: s = min(8, log2-compressed duration); +s inside, -6s outside, -1 per silent member
  float s[12], score[12];
  for (int pc = 0; pc < 12; pc++) {
    float d = 0;
    for (int lb = 0; lb < L; lb++) d += fup[lb][pc] + fb[lb][pc];
    s[pc] = fminf(8, log2f(1 + d));
  }
  int bestK = 0;
  for (int k = 0; k < 12; k++) {
    uint16_t m = ht::collMask(k);
    float sc = 0;
    for (int pc = 0; pc < 12; pc++) {
      if (m & (1 << pc)) sc += s[pc] > 0 ? s[pc] : -1;
      else sc -= 6 * s[pc];
    }
    score[k] = sc;
    r.listenL[k] = 0.5f * sc;
    if (sc > score[bestK]) bestK = k;
  }
  r.coll = forceColl >= 0 ? forceColl : bestK;
  uint16_t set = ht::collMask(r.coll);

  // ---- home evidence
  for (int lb = 0; lb < L; lb++)
    for (int pc = 0; pc < 12; pc++) {
      r.home.bassDur[pc] += fb[lb][pc];
      r.home.totalDur[pc] += fup[lb][pc] + fb[lb][pc];
    }
  {
    float firstOn = 1e30f;
    int firstPc = -1, lastPc = -1;
    float lastEnd = -1;
    float loopEnd = (float)((firstBar / L + 1) * L * 16);   // the end of the first full lap
    for (int i = 0; i < n; i++) {
      if (!ok[i] || !isBass[i]) continue;
      int pc = ev[i].pitch % 12;
      int bar = (int)floorf(ev[i].on / 16);
      float pos = ev[i].on - bar * 16;
      int lb = bar % L;
      if (pos < 1 || (ev[i].on < bar * 16 + 0.5f)) r.home.barStart[pc] += lb == 0 ? 2 : 1;   // loop start counts double
      if (lb == 0 && ev[i].on < firstOn && bar - firstBar < L) firstOn = ev[i].on, firstPc = pc;
      float end = ev[i].on + ev[i].dur;
      if (end <= loopEnd + 0.5f && end > lastEnd && ev[i].dur >= 4) lastEnd = end, lastPc = pc;
    }
    if (firstPc >= 0) r.home.first[firstPc] = 1;
    if (lastPc >= 0) r.home.phraseEnd[lastPc] = 1;
  }

  // ---- bassline only: plain and twist
  float upperSum = 0, bassSum = 0;
  for (int lb = 0; lb < L; lb++)
    for (int pc = 0; pc < 12; pc++) upperSum += fup[lb][pc], bassSum += fb[lb][pc];
  r.bassOnly = upperSum == 0 && bassSum > 0;

  for (int lb = 0; lb < L; lb++) {
    int low = -1;
    float lowD = 0;
    for (int pc = 0; pc < 12; pc++)
      if (fb[lb][pc] > lowD) lowD = fb[lb][pc], low = pc;
    r.chord[lb].low = (int8_t)low;
    if (!r.bassOnly || low < 0) continue;
    BarChord& p = r.plain[lb];
    p.root = (int8_t)low;
    p.low = (int8_t)low;
    p.qual = (set & (1 << mod12(low + 4))) ? ht::Q_BRIGHT : (set & (1 << mod12(low + 3))) ? ht::Q_DARK : ht::Q_POWER;
    BarChord& t = r.twist[lb];
    t.low = (int8_t)low;
    int up4 = mod12(low - 4), up3 = mod12(low - 3), up7 = mod12(low - 7);
    auto inSet = [&](int root, int third) {
      return (set & (1 << root)) && (set & (1 << mod12(root + third))) && (set & (1 << mod12(root + 7)));
    };
    if (inSet(up4, 4)) t.root = (int8_t)up4, t.qual = ht::Q_BRIGHT;          // the bass is its third
    else if (inSet(up3, 3)) t.root = (int8_t)up3, t.qual = ht::Q_DARK;
    else if (inSet(up7, 3)) t.root = (int8_t)up7, t.qual = ht::Q_DARK;       // or its fifth
    else if (inSet(up7, 4)) t.root = (int8_t)up7, t.qual = ht::Q_BRIGHT;
    else t = p;
    r.chord[lb] = p;
  }
  if (r.bassOnly) return true;

  // ---- chord per bar: 37 candidates, the top 8 kept
  static Cand cands[8][9];
  int nc[8];
  float barW[8];
  for (int lb = 0; lb < L; lb++) {
    float w = 0;
    for (int pc = 0; pc < 12; pc++) w += fu[lb][pc] + 3 * fb[lb][pc];
    barW[lb] = w;
    Cand all[37];
    int na = 0;
    for (int root = 0; root < 12; root++)
      for (uint8_t q : {ht::Q_BRIGHT, ht::Q_DARK, ht::Q_POWER}) {
        Cand c{(int8_t)root, q, 0};
        uint16_t tones = candTones(c);
        float sc = 0;
        for (int pc = 0; pc < 12; pc++) {
          float uu = fu[lb][pc], bb = 3 * fb[lb][pc];   // tuned: bass scaled to the upper parts' metric weight
          if (tones & (1 << pc)) sc += 4 * uu;
          else if (set & (1 << pc)) sc -= 2 * uu, sc -= 4 * bb;   // tuned: a bass note outside the chord
          else sc -= 5 * uu, sc -= 4 * bb;
        }
        int fifth = mod12(root + 7), third = mod12(root + (q == ht::Q_DARK ? 3 : 4));
        sc += 6 * 3 * fb[lb][root] + 2 * 3 * fb[lb][fifth];
        if (q != ht::Q_POWER) sc += 1 * 3 * fb[lb][third];
        if (fu[lb][fifth] + fb[lb][fifth] == 0) sc -= 3;
        if (q != ht::Q_POWER && fu[lb][third] + fb[lb][third] == 0) sc -= 3;
        if ((tones & set) == tones) sc += 0.08f * w;
        c.score = sc;
        all[na++] = c;
      }
    all[na++] = {-1, ht::Q_NONE, 0};                 // no chord
    for (int i = 0; i < na; i++)                     // best first
      for (int j = i + 1; j < na; j++)
        if (all[j].score > all[i].score) {
          Cand t = all[i];
          all[i] = all[j];
          all[j] = t;
        }
    nc[lb] = 0;
    bool hasNone = false;
    for (int i = 0; i < na && nc[lb] < 8; i++) {
      cands[lb][nc[lb]++] = all[i];
      hasNone |= all[i].qual == ht::Q_NONE;
    }
    if (!hasNone) cands[lb][nc[lb]++] = {-1, ht::Q_NONE, 0};
  }

  // ---- smooth across bars: two laps, keep the second so the loop joins
  int T = 2 * L;
  static float val[16][9];
  static int8_t from[16][9];
  for (int t = 0; t < T; t++) {
    int lb = t % L;
    for (int si = 0; si < nc[lb]; si++) {
      const Cand& c = cands[lb][si];
      float best = t == 0 ? 0 : -1e30f;
      int arg = -1;
      if (t > 0) {
        int pl = (t - 1) % L;
        for (int pj = 0; pj < nc[pl]; pj++) {
          const Cand& p = cands[pl][pj];
          float cost = 0;
          if (p.root != c.root || p.qual != c.qual) {
            cost = 0.10f * barW[lb];
            if ((candTones(c) & set) != candTones(c)) cost += 0.10f * barW[lb];
          }
          float v = val[t - 1][pj] - cost;
          if (v > best) best = v, arg = pj;
        }
      }
      val[t][si] = best + c.score;
      from[t][si] = (int8_t)arg;
    }
  }
  int si = 0;
  for (int k = 1; k < nc[(T - 1) % L]; k++)
    if (val[T - 1][k] > val[T - 1][si]) si = k;
  for (int t = T - 1; t >= L; t--) {
    int lb = t % L;
    const Cand& c = cands[lb][si];
    BarChord& out = r.chord[lb];
    out.root = c.root;
    out.qual = c.qual;
    out.ext = 0;
    if (c.qual == ht::Q_POWER) {                     // no third heard: fill it from the note set
      if (set & (1 << mod12(c.root + 3))) out.qual = ht::Q_DARK;
      else if (set & (1 << mod12(c.root + 4))) out.qual = ht::Q_BRIGHT;
      float w4 = fu[lb][mod12(c.root + 5)] + 3 * fb[lb][mod12(c.root + 5)];
      if (barW[lb] > 0 && w4 >= 0.15f * barW[lb]) out.qual = ht::Q_HANG;
    }
    if (c.root >= 0 && barW[lb] > 0) {               // sevenths, ninths, sus: 15% of the bar
      auto share = [&](int iv) { return (fu[lb][mod12(c.root + iv)] + 3 * fb[lb][mod12(c.root + iv)]) / barW[lb]; };
      if (share(10) >= 0.15f || share(11) >= 0.15f) out.ext |= X_7;
      if (share(2) >= 0.15f) out.ext |= X_9;
      if (out.qual == ht::Q_HANG) out.ext |= X_SUS;
      if (out.qual == ht::Q_BRIGHT && share(10) >= 0.15f) out.qual = ht::Q_DOM;
    }
    si = from[t][si] >= 0 ? from[t][si] : 0;
  }
  return true;
}

bool detectTempo(const uint32_t* on, const uint8_t* weight, int n, float& bpm, float& phaseUs, int& downbeat,
                 float& lead) {
  if (n < 8) return false;
  float bestScore = -1;
  for (int tb = 115; tb <= 128; tb++) {
    float per = 60e6f / tb / 4;
    for (int ph = 0; ph < 16; ph++) {
      float off = per * ph / 16, sc = 0;
      for (int i = 0; i < n; i++) {
        float x = fmodf((float)on[i] - off, per);
        if (x < 0) x += per;
        float d = fminf(x, per - x);
        if (d <= 30000) sc += 1 - d / 30000;         // within 30 ms of the 16th grid
      }
      if (sc > bestScore) bestScore = sc, bpm = (float)tb, phaseUs = off;
    }
  }
  // the downbeat: which 16th of the bar the weighted onsets favour
  float per = 60e6f / bpm / 4, sc[16] = {0};
  for (int d = 0; d < 16; d++)
    for (int i = 0; i < n; i++) {
      int step = (int)lroundf(((float)on[i] - phaseUs) / per);
      sc[d] += metricW(((step - d) % 16 + 16) % 16) * (weight ? weight[i] : 1);
    }
  int b1 = 0, b2 = -1;
  for (int d = 1; d < 16; d++)
    if (sc[d] > sc[b1]) b1 = d;
  for (int d = 0; d < 16; d++)
    if (d != b1 && (b2 < 0 || sc[d] > sc[b2])) b2 = d;
  downbeat = b1;
  lead = sc[b1] > 0 ? (sc[b1] - sc[b2]) / sc[b1] : 0;
  return true;
}

}  // namespace hl
