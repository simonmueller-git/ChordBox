# ChordBox — UX spec v5 "chord discovery"

One job: find chords and chord progressions fast. No drum machine, no
sequencer, no tempo, no swing, no melody line.

**Nothing is quantised.** A chord sounds the instant the pad goes down. The
Move's MIDI clock is read and thrown away, so tempo never decides when a note
happens — you can play freely over a running loop.

Hardware constraint: the LEFT keypad's two middle rows are dead. Layout:
**parameters = SHIFT + RIGHT keypad** (full 4x4), **commands = SHIFT + LEFT
top/bottom rows**, chord pages are 6 rows.

SHIFT = GP9. MIDI out: chords ch1, bass ch2.

## Global grammar

| Control            | Action |
|--------------------|--------|
| Pad (unshifted)    | Play a chord — immediately, held until release |
| Encoder turn       | **PAGE** (home). Picking another param borrows it for 8 s, then it falls back to PAGE |
| Encoder push+turn  | **BANK** — always, whatever is focused |
| Encoder click      | Cycle PAGE > BANK > ROOT > DICE |
| SHIFT + right pad  | Focus a param (4x4 legend on the OLED while held) |
| SHIFT + left pad   | Command (top row / bottom row only) |
| CONFIRM tap        | Reroll — new variations of every chord |
| CONFIRM hold       | Latch on/off |
| SHIFT + CONFIRM    | Star: save the last played row to favs.txt |
| BACK tap           | All notes off, latch off |
| BACK hold          | PLAIN — reset dice / hum / strum / voicing / bass |

BANK and PAGE are always one gesture away: the encoder returns to PAGE by
itself, and push+turn is BANK no matter what.

## Pads

6 rows per page, 4 chords per row:

```
row 1  left keypad, top row       row 3  right keypad, row 1
row 2  left keypad, bottom row    row 4  right keypad, row 2
                                  row 5  right keypad, row 3
                                  row 6  right keypad, row 4
```

Column = chord 1-4 of that progression. Hold a pad to sustain, release to
stop; with LTCH on, a pad stays down until you tap it again or press another.

## Parameters (SHIFT + RIGHT keypad)

```
         col1      col2      col3      col4
row1     ROOT      OCT       BANK      PAGE
row2     DICE      VOIC      STRM      HUM
row3     BASS      VLD       VEL       —
row4     DARK      TENS      LEN       —
```

- **ROOT / OCT** — key and octave (-2..+2).
- **BANK / PAGE** — 24 banks from progs.txt (Major, Minor, Modal, House,
  Soul, Jazz, LoFi, …) plus **Favs** and the session's **New** bank.
- **DICE** 0-100% — harmonic dice: chord substitutions, added 7ths/9ths,
  voicing scatter. Same pad + same seed always gives the same chord, so a
  progression stays repeatable until you reroll.
- **VOIC** — cls (close) / opn (drop-2) / shl (1-3-7 shell) / st9
  (rootless 3-7-9).
- **STRM** 0-100 — strum spread, 0 = block chord, 100 ≈ 45 ms per note.
  Wall-clock, not clock-synced.
- **HUM** 0-100% — velocity and micro-timing feel.
- **BASS** — off / -12 / -24 / pwr (root + fifth below), on ch2.
- **VLD** — voice leading between consecutive chords.
- **VEL** — base velocity.
- **DARK / TENS / LEN** — generator biases: minor-and-borrowed pull, how many
  7ths/9ths, and 4 / 8 / 16 chords per generated progression.

## Commands (SHIFT + LEFT)

```
top:     STAR      GEN       LTCH      PANC
bottom:  PLAIN     —         —         —
```

- **GEN** builds a progression from house-shaped loop templates (not a random
  walk), drops it into the session **New** bank and jumps there so you can
  play it straight away. 8 and 16-chord progressions become 2 or 4 pad rows.
- **STAR** appends the last played row to favs.txt (needs performance mode)
  and to the Favs bank in RAM.
- **PLAIN** resets dice / hum / strum / voicing / bass — for A/B-ing the
  plain version of a progression against the diced one.

## Display

```
Ab+0 House     2/5      key+octave, bank, page
Fm9      iv            last chord: absolute name + roman ('*' = latch)
 1name      3name      the 6 rows of this page, laid out like the pads
 2name      4name
            5name
            6name
```

While SHIFT is held the screen shows the command + parameter legend instead.

## Files

- progs.txt — the progression library (24 banks)
- favs.txt — starred rows, auto-loaded as a "Favs" bank
- boot.py — storage mode: default = computer mode (Mac writes, device saves
  are RAM-only + "RO!" message). Hold CONFIRM during power-up = performance
  mode (device can save; the drive is read-only to a computer).
- rhythms.txt — unused by v5, left on disk
- code_v4_backup.py — the previous two-program firmware
