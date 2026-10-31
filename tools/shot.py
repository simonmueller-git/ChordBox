#!/usr/bin/env python3
"""
shot.py — grab the ChordBox framebuffer as a PNG (works with or without the LCD).

    python3 tools/shot.py [port] [out.png] [--send "pad 3" --send "knob 5 10" ...]

Commands given with --send run first (with a short pause between), so you can
stage a screen and capture it in one go.
"""

import argparse
import glob
import sys
import time

import serial
from PIL import Image


def find_port() -> str:
    ports = sorted(glob.glob("/dev/cu.usbmodem*")) + sorted(glob.glob("/dev/cu.usbserial-*"))
    if not ports:
        sys.exit("no serial port found")
    return ports[0]


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("port", nargs="?")
    ap.add_argument("out", nargs="?", default="shot.png")
    ap.add_argument("--send", action="append", default=[])
    ap.add_argument("--wait", type=float, default=0.25)
    ap.add_argument("--scale", type=int, default=2)
    a = ap.parse_args()

    s = serial.Serial(a.port or find_port(), 115200, timeout=3)
    s.dtr = True
    time.sleep(0.2)
    s.reset_input_buffer()
    for cmd in a.send:
        s.write((cmd + "\n").encode())
        time.sleep(a.wait)
    time.sleep(0.15)
    s.reset_input_buffer()
    s.write(b"shot\n")
    header = b""
    t0 = time.time()
    while not header.startswith(b"SHOT"):
        header = s.readline()
        if time.time() - t0 > 5:
            sys.exit("no SHOT header")
    _, w, h, fmt = header.decode().split()
    w, h = int(w), int(h)
    raw = b""
    while len(raw) < w * h * 2:
        chunk = s.read(w * h * 2 - len(raw))
        if not chunk:
            sys.exit(f"short read: {len(raw)} of {w * h * 2}")
        raw += chunk
    img = Image.new("RGB", (w, h))
    px = img.load()
    for i in range(w * h):
        v = (raw[2 * i] << 8) | raw[2 * i + 1]          # RGB565, big endian
        r, g, b = (v >> 11) & 31, (v >> 5) & 63, v & 31
        px[i % w, i // w] = (r * 255 // 31, g * 255 // 63, b * 255 // 31)
    if a.scale > 1:
        img = img.resize((w * a.scale, h * a.scale), Image.NEAREST)
    img.save(a.out)
    print(a.out)


if __name__ == "__main__":
    main()
