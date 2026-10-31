#!/usr/bin/env python3
"""
ref.py — reference output from the ORIGINAL ChordBox v5 functions in code.py.

The chord functions are lifted out of code.py with `ast` and executed against
a stand-in `state`, so this is the Pico's own logic, not a rewrite. Prints the
same line format as test_engine.cpp for a straight diff.
"""
import ast
import random
from pathlib import Path

CHORDBOX = Path(__file__).resolve().parents[3]
src = (CHORDBOX / "code.py").read_text()
tree = ast.parse(src)

WANT_ASSIGN = {"NOTE_NAMES", "CHORDS", "SUBS", "_SUFFIXES", "ROWS_PER_PAGE"}
WANT_FUNC = {"_base_triad", "chord_symbol", "_load_progs", "bank_pages", "row_ref",
             "row_tokens", "_substitute", "_add_extension", "_randomize_voicing",
             "_voice_lead", "apply_voicing", "build"}
nodes = []
for n in tree.body:
    if isinstance(n, ast.Assign) and any(getattr(t, "id", None) in WANT_ASSIGN for t in n.targets):
        nodes.append(n)
    elif isinstance(n, ast.FunctionDef) and n.name in WANT_FUNC:
        nodes.append(n)
g = {"random": random, "state": {}, "last_voicing": None, "session_seed": 0,
     "__name__": "ref"}
exec(compile(ast.Module(body=nodes, type_ignores=[]), "code.py", "exec"), g)

import os
os.chdir(CHORDBOX)                              # _load_progs reads progs.txt
PROGS, BANKS = g["_load_progs"]()
g["PROGS"], g["BANKS"] = PROGS, BANKS
g["ROWS_PER_PAGE"] = 4                         # the S3 shows 4 rows per page

banks = [1, 2, 11, 13, 18]
roots = [5, 9]
bass = 0                                       # the S3 engine no longer doubles the bass
for voic in range(4):
    for root in roots:
        for b in banks:
            g["state"].update({"root_pc": root, "octave": 0, "voic": voic, "bass": bass,
                               "dice": 0, "vlead": 1, "bank": b})
            g["last_voicing"] = None
            pages = g["bank_pages"](b)
            for pg in range(pages):
                for pad in range(16):
                    absrow, col = pg * 4 + pad // 4, pad % 4
                    toks = g["row_tokens"](b, absrow)
                    if not toks:
                        continue
                    roman = toks[col % len(toks)]
                    ref = g["row_ref"](b, absrow)
                    notes, root_note, sym, rn = g["build"](roman, ref[0] * 977 + ref[1] * 131 + col * 29)
                    if not notes:
                        continue
                    print(f"v{voic} r{root} bank{b} row{absrow} col{col} |"
                          + "".join(f" {n}" for n in sorted(notes))
                          + f" | {sym}")
