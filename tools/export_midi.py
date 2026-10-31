#!/usr/bin/env python3
"""
export_midi.py: write every progression in the chord banks as a .mid file.

    python3 tools/export_midi.py        -> midi/<Bank>/<number> <chords>.mid

Same data as the firmware: pico/progs.txt (progressions and banks) and CHORDS in
pico/code.py (each chord as semitones above the key). Files are in C, one chord
per bar, root position from C3, on MIDI channel 1 at 124 BPM. The tonic is always
C, so a minor progression is in C minor. Drop a file on a clip and transpose it
to your key. No dependencies.
"""
import ast
import shutil
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "midi"
PPQ, BPM, BASE, VEL = 480, 124, 48, 90
BANK_NAMES = {
    "Romant": "Romantic", "Nostlg": "Nostalgic", "Triumf": "Triumphant",
    "Surpr": "Surprised", "Myst": "Mysterious", "Cadenc": "Cadence",
    "Play": "Playful", "Rebel": "Rebellious",
}


def vlq(n):
    out = [n & 0x7F]
    while n > 0x7F:
        n >>= 7
        out.insert(0, (n & 0x7F) | 0x80)
    return bytes(out)


def smf(chords):
    """One track: tempo, then the chords one bar each."""
    t = vlq(0) + b"\xff\x51\x03" + (60_000_000 // BPM).to_bytes(3, "big")
    t += vlq(0) + b"\xff\x58\x04\x04\x02\x18\x08"
    for iv in chords:
        notes = [BASE + i for i in iv]
        for n in notes:
            t += vlq(0) + bytes((0x90, n, VEL))
        for k, n in enumerate(notes):
            t += vlq(PPQ * 4 if k == 0 else 0) + bytes((0x80, n, 0))
    t += vlq(0) + b"\xff\x2f\x00"
    return b"MThd" + struct.pack(">IHHH", 6, 0, 1, PPQ) + b"MTrk" + struct.pack(">I", len(t)) + t


def main():
    tree = ast.parse((ROOT / "pico/code.py").read_text())
    env = {}
    for node in tree.body:
        if isinstance(node, ast.Assign) and getattr(node.targets[0], "id", "") in ("CHORDS", "NOTE_NAMES"):
            env[node.targets[0].id] = ast.literal_eval(node.value)
        if isinstance(node, ast.FunctionDef) and node.name == "chord_symbol":
            exec(compile(ast.Module([node], []), "code.py", "exec"), env)   # its naming, nothing else
    progs, banks = [], []
    for line in (ROOT / "pico/progs.txt").read_text().splitlines():
        if not line.strip() or line.startswith("#"):
            continue
        kind, a, b = line.split("\t")
        if kind == "P":
            progs.append(b.split())
        elif kind == "B":
            banks.append((BANK_NAMES.get(a, a), [int(x) for x in b.split(",")]))
    if OUT.exists():
        shutil.rmtree(OUT)
    total = 0
    for bank, rows in banks:
        (OUT / bank).mkdir(parents=True)
        seen = []
        for r in rows:                              # a row is prog * 2 + half: keep whole progressions
            if r >> 1 not in seen:
                seen.append(r >> 1)
        for n, p in enumerate(seen, 1):
            ivs = [env["CHORDS"][t] for t in progs[p]]
            names = " ".join(env["chord_symbol"](0, iv) for iv in ivs)
            (OUT / bank / f"{n:03d} {names}.mid").write_bytes(smf(ivs))
            total += 1
    print(f"{total} files in {len(banks)} banks -> {OUT}")


if __name__ == "__main__":
    main()
