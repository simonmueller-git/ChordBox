#!/usr/bin/env python3
"""
midi_test.py — end-to-end check on the device: drive both programs through the
serial console and confirm the right MIDI comes out of the "ChordBox" USB-MIDI
port (chords on channel 1, bass on channel 2). Needs no keypads, knob or Move.
Puts the settings it touches back to their defaults at the end.

    python3 tools/midi_test.py [serial-port]

Clock timing against a simulated Move is tools/clock_test.py.
"""

import glob
import sys
import time

import mido
import serial

CHORD, BASS = 0, 1          # MIDI channels, 0-based
STEP = 60 / 120 / 4         # a 16th at the 120 BPM set below


def main() -> None:
    names = [n for n in mido.get_input_names() if "ChordBox" in n.lower()]
    if not names:
        sys.exit(f"no ChordBox MIDI port; inputs: {mido.get_input_names()}")
    port = sys.argv[1] if len(sys.argv) > 1 else sorted(glob.glob("/dev/cu.usbmodem*"))[0]
    s = serial.Serial(port, 115200, timeout=1)
    s.dtr = True
    time.sleep(0.3)
    fails = []

    with mido.open_input(names[0]) as mi:
        def drain(t=0.4):
            out, end = [], time.time() + t
            while time.time() < end:
                for m in mi.iter_pending():
                    out.append((time.time(), m))
                time.sleep(0.001)
            return out

        def cmd(*cs):
            for c in cs:
                s.write((c + "\n").encode())
                time.sleep(0.02)

        def ons(got, ch):
            return [(t, m) for t, m in got if m.type == "note_on" and m.velocity > 0 and m.channel == ch]

        def offs(got, ch):
            return [m for _, m in got if m.channel == ch and
                    (m.type == "note_off" or (m.type == "note_on" and m.velocity == 0))]

        def check(name, ok):
            if not ok:
                fails.append(name)

        drain(0.3)
        cmd("act alloff", "set DICE 0", "set STRM 0", "set VEL 90",
            "bset TEMPO 120", "bset GENRE 0", "bset FOLLOW 0", "bset SWING 50")
        drain(0.5)

        # 1. block chord on channel 1 at the set velocity; release sends as many note-offs
        cmd("pad 1")
        a = ons(drain(0.3), CHORD)
        print(f"1 chord     {[(m.note, m.velocity) for _, m in a]}")
        check("chord", len(a) >= 3 and all(m.velocity == 90 for _, m in a))
        cmd("up 1")
        o = offs(drain(0.3), CHORD)
        print(f"  release   {len(o)} note-offs")
        check("release", len(o) == len(a))

        # 2. strum spreads the notes in time (45 ms per note at 100)
        cmd("set STRM 100")
        drain(0.2)
        cmd("pad 2")
        a = ons(drain(0.6), CHORD)
        span = (a[-1][0] - a[0][0]) * 1000 if len(a) > 1 else 0
        print(f"2 strum     {len(a)} notes over {span:.0f} ms")
        check("strum", len(a) > 1 and span >= 0.7 * 45 * (len(a) - 1))
        cmd("up 2", "set STRM 0")
        drain(0.4)

        # 3. latch: a chord keeps sounding after release; the next one replaces it
        cmd("act latch")
        drain(0.2)
        cmd("pad 4", "up 4")
        first = drain(0.3)
        cmd("pad 5", "up 5")
        second = drain(0.3)
        print(f"3 latch     offs after 1st {len(offs(first, CHORD))} (want 0), after 2nd {len(offs(second, CHORD))} (want >0)")
        check("latch", not offs(first, CHORD) and offs(second, CHORD))
        cmd("act latch", "act alloff")
        drain(0.4)

        # 4. same pad + same VAR = same chord, even with DICE at 100
        cmd("set DICE 100", "set VAR 3")
        drain(0.2)
        pcs = []
        for _ in range(2):
            cmd("pad 6")
            pcs.append(sorted({m.note % 12 for _, m in ons(drain(0.25), CHORD)}))
            cmd("up 6")
            drain(0.2)
        print(f"4 dice      {pcs[0]} / {pcs[1]}")
        check("dice-repeat", pcs[0] and pcs[0] == pcs[1])
        cmd("set DICE 0", "set VAR 0")
        drain(0.3)

        # 5. bass: BASS screen, pad 1 (ROLLER) previews at 120 BPM on channel 2, on the 16th grid
        cmd("click", "pad 1")
        got = drain(2.1)
        b = ons(got, BASS)
        grid = [((t - b[0][0]) / STEP) for t, _ in b] if b else []
        off_grid = max((abs(g - round(g)) * STEP * 1000 for g in grid), default=99)
        print(f"5 bass      {len(b)} notes in 2 s on channel 2, worst {off_grid:.1f} ms off the grid")
        check("bass-preview", len(b) >= 10 and off_grid < 8 and not ons(got, CHORD))

        # 6. back on CHORD the bass keeps running and follows the next chord
        cmd("click")
        drain(0.3)
        t0 = time.time()
        cmd("pad 3")
        got = drain(1.2)
        chord_pcs = {m.note % 12 for _, m in ons(got, CHORD)}
        bass_pcs = {m.note % 12 for t, m in ons(got, BASS) if t > t0 + 2 * STEP}
        print(f"6 follow    chord {sorted(chord_pcs)}, bass now {sorted(bass_pcs)}")
        check("bass-follow", bass_pcs and bass_pcs <= chord_pcs)
        cmd("up 3")
        drain(0.2)

        # 7. tapping the playing line again stops the preview
        cmd("click", "pad 1")
        drain(0.4)
        late = ons(drain(0.8), BASS)
        print(f"7 stop      bass notes after stop: {len(late)} (want 0)")
        check("bass-stop", not late)
        cmd("click")
        drain(0.2)

        # 8. PANIC sends all-notes-off
        cmd("act panic")
        got = drain(0.4)
        cc = [m for _, m in got if m.type == "control_change" and m.control == 123]
        print(f"8 panic     all-notes-off CCs {len(cc)}")
        check("panic", cc)

        cmd("set VEL 100", "bset TEMPO 124", "act alloff")
        drain(0.3)

    print("PASS" if not fails else f"FAIL: {', '.join(fails)}")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
