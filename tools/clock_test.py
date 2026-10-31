#!/usr/bin/env python3
"""
clock_test.py — play the Move's part from the Mac: send START + MIDI clock to
"ChordBox chords" over USB and measure how tightly the bass that comes back
sits on the clock. Receive times come from CoreMIDI's own timestamps
(python-rtmidi deltas), so Python's thread switching doesn't blur them.

    python3 tools/clock_test.py [bpm] [seconds]
"""

import glob
import statistics
import sys
import threading
import time

import rtmidi
import serial

BPM = float(sys.argv[1]) if len(sys.argv) > 1 else 124.0
SECS = float(sys.argv[2]) if len(sys.argv) > 2 else 8.0


def open_port(cls, needle):
    p = cls()
    for i, n in enumerate(p.get_ports()):
        if needle in n.lower():
            p.open_port(i)
            return p
    sys.exit(f"no {needle} port")


def main() -> None:
    sys.setswitchinterval(0.0002)
    out = open_port(rtmidi.MidiOut, "ChordBox")
    mi = open_port(rtmidi.MidiIn, "ChordBox")
    mi.ignore_types(sysex=True, timing=False, active_sense=True)
    con = serial.Serial(sorted(glob.glob("/dev/cu.usbmodem*"))[0], 115200, timeout=0.3)
    con.dtr = True
    time.sleep(0.5)
    for c in ["bset GENRE 0", "bset SWING 50", "bset FOLLOW 0", "bset NUDGE 0"]:
        con.write((c + "\n").encode())
        time.sleep(0.05)

    recv = []                                   # (seconds since first message, bytes)
    clock_t = [0.0]

    def on_msg(event, _):
        msg, delta = event
        clock_t[0] += delta
        recv.append((clock_t[0], time.perf_counter(), msg))

    mi.set_callback(on_msg)
    tick = 60.0 / (BPM * 24)
    sent = []
    stop = threading.Event()

    def clock():
        out.send_message([0xFA])
        t0 = time.perf_counter()
        n = 0
        while not stop.is_set():
            target = t0 + n * tick
            while time.perf_counter() < target - 0.002:
                time.sleep(0.0005)
            while time.perf_counter() < target:
                pass
            out.send_message([0xF8])
            sent.append(time.perf_counter())
            n += 1

    th = threading.Thread(target=clock, daemon=True)
    th.start()
    time.sleep(SECS)
    stop.set()
    th.join()
    out.send_message([0xFC])
    time.sleep(0.4)

    bass = [(ct, pt, m) for ct, pt, m in recv if len(m) == 3 and (m[0] & 0xF0) == 0x90 and m[2] and (m[0] & 15) == 1]
    offs = [m for _, _, m in recv if len(m) == 3 and (m[0] & 15) == 1 and ((m[0] & 0xF0) == 0x80 or ((m[0] & 0xF0) == 0x90 and not m[2]))]
    print(f"bass notes: {len(bass)}   note-offs: {len(offs)}   ({SECS:.0f} s at {BPM:g} BPM)")
    if len(bass) < 8:
        print("FAIL: too few bass notes")
        sys.exit(1)
    # map CoreMIDI time onto the sender's clock: offset from the median of perf_counter - coremidi
    off = statistics.median(pt - ct for ct, pt, _ in bass)
    step_ticks = sent[::6]                      # every 6th clock = a 16th boundary, as sent
    lag = []
    for ct, _, _ in bass:
        t = ct + off
        near = min(step_ticks, key=lambda s: abs(s - t))
        lag.append((t - near) * 1000)
    base = statistics.median(lag)
    jitter = statistics.pstdev(lag)
    spread = max(lag) - min(lag)
    print(f"timing vs the clock it received: jitter (sd) {jitter:.2f} ms, spread {spread:.1f} ms "
          f"(constant offset {base:.1f} ms includes USB both ways)")
    pcs = sorted({m[1] % 12 for _, _, m in bass})
    print(f"bass pitch classes: {pcs}")
    ok = jitter < 1.5 and len(offs) >= len(bass) - 1
    print("PASS" if ok else "FAIL")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
