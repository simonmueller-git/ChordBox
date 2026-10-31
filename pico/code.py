# code.py — ChordBox v5 "chord discovery"
# Raspberry Pi Pico / CircuitPython — a chord + chord-progression
# discovery instrument. One program, no sequencer, no drum machine.
#
# NOTHING IS QUANTISED. Chords sound the instant you press a pad. The MIDI
# clock coming from the Move is ignored completely — tempo/swing do not
# exist here, so nothing waits for a downbeat or an offbeat.
#
# HARDWARE NOTE: the left keypad's two middle rows are dead. Everything is
# mapped to: left rows 1+4, the full right keypad, encoder, SHIFT (GP9),
# BACK (GP3), CONFIRM (GP2).
#
# GRAMMAR
#   pads (unshifted)   play a chord. 6 rows x 4 columns per page:
#                      left top row = row 1, left bottom row = row 2,
#                      right rows 1-4 = rows 3-6. Column = chord 1-4.
#   encoder turn       PAGE (home). After picking another parameter the
#                      encoder edits that one, then falls back to PAGE.
#   encoder push+turn  BANK — always, whatever is focused.
#   encoder click      cycle PAGE > BANK > ROOT > DICE
#   SHIFT + right pad  focus a parameter (4x4 legend shows while held)
#   SHIFT + left pad   command (top row / bottom row only)
#   CONFIRM tap        reroll — new variations of every chord
#   CONFIRM hold       latch on/off
#   SHIFT + CONFIRM    star: save the last played row to favs.txt
#   BACK tap           all notes off
#   BACK hold          PLAIN — reset dice/hum/strum/voicing/bass
#
# MIDI OUT: chords ch1, bass ch2.

import time
import random
import gc
import board
import digitalio
import busio
import displayio
import terminalio
import rotaryio
import usb_midi
import adafruit_midi
from adafruit_midi.note_on import NoteOn
from adafruit_midi.note_off import NoteOff
from adafruit_display_text import label
import adafruit_displayio_sh1106


# =========================
# PIN MAP
# =========================

PIN_SDA = board.GP4
PIN_SCL = board.GP5

PIN_ENC_A   = board.GP6
PIN_ENC_B   = board.GP7
PIN_ENC_BTN = board.GP8
PIN_BACK    = board.GP3
PIN_CONFIRM = board.GP2
PIN_SHIFT   = board.GP9

ROW_PINS_LEFT = [board.GP14, board.GP15, board.GP16, board.GP17]
COL_PINS_LEFT = [board.GP10, board.GP11, board.GP12, board.GP13]
ROW_PINS_RIGHT = [board.GP22, board.GP26, board.GP27, board.GP28]
COL_PINS_RIGHT = [board.GP18, board.GP19, board.GP20, board.GP21]


# =========================
# CHORD DATABASE
# =========================

NOTE_NAMES = ('C','Db','D','Eb','E','F','Gb','G','Ab','A','Bb','B')

CHORDS = {
'I':(0, 4, 7), 'I-5':(0, 4, 6), 'I5':(0, 7, 12), 'I7':(0, 4, 7, 10),
'I9':(0, 4, 7, 10, 14), 'II':(2, 6, 9), 'II5':(2, 9, 14), 'III':(4, 8, 11),
'IV':(5, 9, 12), 'IV5':(5, 12, 17), 'IV6':(5, 9, 12, 14), 'IV7':(5, 9, 12, 15),
'IVmaj7':(5, 9, 12, 16), 'IVsus4':(5, 10, 12), 'Iadd9':(0, 4, 7, 14),
'Imaj7':(0, 4, 7, 11), 'Isus2':(0, 2, 7), 'Isus4':(0, 5, 7),
'V':(7, 11, 14), 'V5':(7, 14, 19), 'V6':(7, 11, 14, 16), 'V7':(7, 11, 14, 17),
'V9':(7, 11, 14, 17, 21), 'VI':(9, 13, 16), 'VI7':(9, 13, 16, 19),
'VII':(11, 15, 18), 'VII7':(11, 15, 18, 21), 'VIadd9':(9, 13, 16, 23),
'VIsus4':(9, 14, 16), 'Vadd9':(7, 11, 14, 21), 'Vsus2':(7, 9, 14),
'Vsus4':(7, 12, 14), 'bII':(1, 5, 8), 'bIII':(3, 7, 10),
'bIII6':(3, 7, 10, 12), 'bIIImaj7':(3, 7, 10, 14), 'bIIIsus2':(3, 5, 10),
'bIImaj7':(1, 5, 8, 12), 'bVI':(8, 12, 15), 'bVI69':(8, 12, 15, 17, 22),
'bVII':(10, 14, 17), 'bVII6':(10, 14, 17, 19), 'bVIImaj7':(10, 14, 17, 21),
'bVIIsus4':(10, 15, 17), 'bVIadd9':(8, 12, 15, 22), 'bVImaj7':(8, 12, 15, 19),
'bii':(1, 4, 8), 'bii6':(1, 4, 8, 10), 'biii':(3, 6, 10), 'bv':(6, 9, 13),
'bv0':(6, 9, 12), 'bvii':(10, 13, 17), 'i':(0, 3, 7), 'i7':(0, 3, 7, 10),
'ii':(2, 5, 9), 'ii0':(2, 5, 8), 'ii7':(2, 5, 9, 12), 'ii9':(2, 5, 9, 12, 16),
'iii':(4, 7, 11), 'iii7':(4, 7, 11, 14), 'iv':(5, 8, 12), 'iv6':(5, 8, 12, 14),
'iv7':(5, 8, 12, 15), 'iv9':(5, 8, 12, 15, 19), 'ivadd9':(5, 8, 12, 19),
'v':(7, 10, 14), 'v0':(7, 10, 13), 'v7':(7, 10, 14, 17), 'vi':(9, 12, 16),
'vi7':(9, 12, 16, 19), 'vii':(11, 14, 18), 'vii0':(11, 14, 17),
'vii7':(11, 14, 17, 20),
}

SUBS = {
    'I':['vi','iii','Imaj7','I7'], 'i':['bIII','i7','bVI'],
    'ii':['IV','ii7'], 'ii7':['IV','ii9','IVmaj7'],
    'iii':['I','vi','iii7'], 'iii7':['I','vi7'],
    'IV':['ii','IVmaj7','ii7'], 'IVmaj7':['ii7','vi7'],
    'V':['vii0','V7','iii'], 'V7':['vii0','V9','bVII'],
    'vi':['I','vi7','IV'], 'vi7':['I','IVmaj7'],
    'i7':['bIII','iv7'], 'iv':['bVI','iv7'], 'iv7':['bVI'],
    'bVI':['iv','bIII'], 'bVII':['V','bIII'],
    'I7':['IV7','I'], 'IV7':['I7','ii7'], 'v7':['bVII','i7'],
    'VI7':['ii7','iv'], 'v':['v7','bVII'],
    'II':['IV','V'], 'III':['V','I'], 'VI':['IV','vi'],
    'VII':['V','bVII'], 'bII':['bVI','V7'], 'bIII':['bVI','i'],
    'ii0':['iv','ii7'], 'v0':['V','bVII'], 'bv':['v','ii0'],
    'biii':['bIII','i'], 'bvii':['bVII','v'], 'bii':['bII','iv'],
}

_SUFFIXES = ('maj9','maj7','add9','sus2','sus4','dim7','69','07',
             '9','7','6','5','-5','0')

def _base_triad(tok):
    for s in _SUFFIXES:
        if tok.endswith(s):
            return tok[:-len(s)]
    return tok

def chord_symbol(key_pc, iv):
    """Absolute name of a chord from its key-relative intervals."""
    r = (key_pc + iv[0]) % 12
    pcs = [(x - iv[0]) % 12 for x in iv]
    has = lambda x: x in pcs
    third = 3 if has(3) else (4 if has(4) else None)
    if third == 3:
        q = 'dim' if (has(6) and not has(7)) else 'm'
    elif third == 4:
        q = 'aug' if (has(8) and not has(7)) else ''
    else:
        q = 'sus2' if has(2) else ('sus4' if has(5) else '5')
    if q == 'dim':
        if has(10):
            return NOTE_NAMES[r] + 'm7b5'
        if has(9):
            return NOTE_NAMES[r] + 'dim7'
        return NOTE_NAMES[r] + 'dim'
    if has(11):
        e = 'maj9' if has(2) else 'maj7'
    elif has(10):
        e = '9' if has(2) else '7'
    elif has(9) and third is not None:
        e = '6'
    elif has(2) and third is not None:
        e = 'add9'
    else:
        e = ''
    return NOTE_NAMES[r] + q + e


# =========================
# LIBRARY (progs.txt / favs.txt)
# =========================

def _load_progs():
    progs = []
    banks = []
    with open('progs.txt') as f:
        for line in f:
            line = line.strip()
            if not line or line[0] == '#':
                continue
            kind, a, b = line.split('\t')
            if kind == 'P':
                progs.append((a, b))
            elif kind == 'B':
                banks.append((a, [int(x) for x in b.split(',')]))
    return progs, banks

PROGS, BANKS = _load_progs()

def _load_favs():
    rows = []
    try:
        with open('favs.txt') as f:
            for line in f:
                line = line.strip()
                if not line or line[0] == '#':
                    continue
                _, name, toks = line.split('\t')
                idx = len(PROGS)
                PROGS.append((name, toks))
                rows.append(idx * 2)
                if len(toks.split()) > 4:
                    rows.append(idx * 2 + 1)
    except OSError:
        pass
    if rows:
        BANKS.append(('Favs', rows))

_load_favs()
gc.collect()

ROWS_PER_PAGE = 6

def bank_pages(bank_idx):
    n = len(BANKS[bank_idx][1])
    return max(1, (n + ROWS_PER_PAGE - 1) // ROWS_PER_PAGE)

def row_ref(bank_idx, row_idx):
    rows = BANKS[bank_idx][1]
    if row_idx >= len(rows):
        return None
    v = rows[row_idx]
    return v >> 1, v & 1

def row_name(bank_idx, row_idx):
    ref = row_ref(bank_idx, row_idx)
    if ref is None:
        return ''
    p, half = ref
    nf = PROGS[p][0]
    if '|' in nf:
        return nf.split('|')[half]
    return nf

def row_tokens(bank_idx, row_idx):
    """The 4 chords of one pad row."""
    ref = row_ref(bank_idx, row_idx)
    if ref is None:
        return []
    p, half = ref
    t = PROGS[p][1].split()
    return t[half * 4:half * 4 + 4] if len(t) > 4 else t


# =========================
# HARDWARE INIT
# =========================

def _make_in(pin):
    io = digitalio.DigitalInOut(pin)
    io.direction = digitalio.Direction.INPUT
    io.pull = digitalio.Pull.UP
    return io

def _make_out_hi(pin):
    io = digitalio.DigitalInOut(pin)
    io.direction = digitalio.Direction.OUTPUT
    io.value = True
    return io

rows_left  = [_make_out_hi(p) for p in ROW_PINS_LEFT]
rows_right = [_make_out_hi(p) for p in ROW_PINS_RIGHT]
cols_left  = [_make_in(p) for p in COL_PINS_LEFT]
cols_right = [_make_in(p) for p in COL_PINS_RIGHT]

enc_btn   = _make_in(PIN_ENC_BTN)
btn_back  = _make_in(PIN_BACK)
btn_conf  = _make_in(PIN_CONFIRM)
btn_shift = _make_in(PIN_SHIFT)

encoder = rotaryio.IncrementalEncoder(PIN_ENC_A, PIN_ENC_B)
last_enc_pos = encoder.position

_port = usb_midi.ports[1] if len(usb_midi.ports) >= 2 else usb_midi.ports[0]
midi  = adafruit_midi.MIDI(midi_out=_port, out_channel=0)   # chords ch1
midi2 = adafruit_midi.MIDI(midi_out=_port, out_channel=1)   # bass   ch2
# MIDI in is drained and thrown away — the clock must never touch playback.
_in_port = usb_midi.ports[0] if len(usb_midi.ports) >= 2 else None

displayio.release_displays()
i2c = busio.I2C(PIN_SCL, PIN_SDA)
try:
    import i2cdisplaybus
    display_bus = i2cdisplaybus.I2CDisplayBus(i2c, device_address=0x3C)
except ImportError:
    display_bus = displayio.I2CDisplay(i2c, device_address=0x3C)
display = adafruit_displayio_sh1106.SH1106(display_bus, width=128, height=64)

gc.collect()
if hasattr(gc, 'mem_free'):
    print('free mem after init:', gc.mem_free())


# =========================
# STATE
# =========================

LEN_OPTS = (4, 8, 16)
VOIC_N = ('cls', 'opn', 'shl', 'st9')
BASS_N = ('off', '-12', '-24', 'pwr')

state = {
    'root_pc': 0,
    'octave':  0,
    'bank':    0,
    'page':    0,
    'dice':    0,     # harmonic dice: substitutions / extensions / voicing
    'voic':    0,     # close / open / shell / rootless 9
    'sprd':    0,     # strum spread, 0 = block chord
    'hum':     0,     # feel: velocity + timing jitter
    'bass':    0,     # off / -12 / -24 / root+5th
    'vlead':   1,     # voice leading
    'vel':     100,   # base velocity
    'dark':    50,    # generator: minor / borrowed bias
    'tens':    30,    # generator: 7ths / 9ths bias
    'len':     4,     # generator: chords per progression
    'latch':   False,
}

last_voicing = None
last_row = 0
last_sym = ''
last_rn  = ''
active = {}        # key_id -> [(note, midi_obj), ...]
sched  = []        # [t_due, note, vel, midi_obj, key_id]  (strum tails)

conf_press_t = None
conf_shift = False
back_press_t = None
shift_down = False
enc_moved = False
msg = ''
msg_until = 0.0
focus = 'page'
focus_until = 0.0
FOCUS_HOLD = 8.0   # seconds before the encoder falls back to PAGE
_was_special = False
ui_dirty = False
gen_count = 0
fav_count = 0

# --- boot randomization: land somewhere different every session ---
try:
    import os
    random.seed(int.from_bytes(os.urandom(4), 'little'))
except (ImportError, NotImplementedError):
    pass
state['root_pc'] = random.randrange(12)
state['bank'] = random.randrange(len(BANKS))
state['page'] = random.randrange(bank_pages(state['bank']))
session_seed = random.randrange(0x8000)


# =========================
# PARAM SYSTEM (shift-grid)
# =========================

GRID = (
    ('root', 'oct',  'bank', 'page'),
    ('dice', 'voic', 'sprd', 'hum'),
    ('bass', 'vlead','vel',  ''),
    ('dark', 'tens', 'len',  ''),
)
GRID_LBL = (
    ('ROOT', 'OCT',  'BANK', 'PAGE'),
    ('DICE', 'VOIC', 'STRM', 'HUM'),
    ('BASS', 'VLD',  'VEL',  ''),
    ('DARK', 'TENS', 'LEN',  ''),
)
CMDS    = (('star', 'gen', 'ltch', 'panc'), ('plain', '', '', ''))
CMD_LBL = (('STAR', 'GEN', 'LTCH', 'PANC'), ('PLAIN', '', '', ''))
VITALS  = ('page', 'bank', 'root', 'dice')

LBL = {}
for _r in range(4):
    for _c in range(4):
        if GRID[_r][_c]:
            LBL[GRID[_r][_c]] = GRID_LBL[_r][_c]

def val_str(name):
    s = state
    if name == 'root':  return NOTE_NAMES[s['root_pc']]
    if name == 'oct':   return '%+d' % s['octave']
    if name == 'bank':  return BANKS[s['bank']][0]
    if name == 'page':  return '%d/%d' % (s['page'] + 1, bank_pages(s['bank']))
    if name == 'dice':  return '%d%%' % s['dice']
    if name == 'voic':  return VOIC_N[s['voic']]
    if name == 'sprd':  return 'off' if s['sprd'] == 0 else '%d' % s['sprd']
    if name == 'hum':   return '%d%%' % s['hum']
    if name == 'bass':  return BASS_N[s['bass']]
    if name == 'vlead': return 'on' if s['vlead'] else 'off'
    if name == 'vel':   return '%d' % s['vel']
    if name == 'dark':  return '%d%%' % s['dark']
    if name == 'tens':  return '%d%%' % s['tens']
    if name == 'len':   return '%d' % s['len']
    return ''

def adjust(name, d):
    s = state
    if not name:
        return
    if name == 'root':
        s['root_pc'] = (s['root_pc'] + d) % 12
    elif name == 'oct':
        s['octave'] = max(-2, min(2, s['octave'] + d))
    elif name == 'bank':
        s['bank'] = (s['bank'] + d) % len(BANKS)
        s['page'] = 0
    elif name == 'page':
        s['page'] = (s['page'] + d) % bank_pages(s['bank'])
    elif name == 'dice':
        s['dice'] = max(0, min(100, s['dice'] + d * 5))
    elif name == 'voic':
        s['voic'] = (s['voic'] + d) % 4
    elif name == 'sprd':
        s['sprd'] = max(0, min(100, s['sprd'] + d * 5))
    elif name == 'hum':
        s['hum'] = max(0, min(100, s['hum'] + d * 5))
    elif name == 'bass':
        s['bass'] = (s['bass'] + d) % 4
    elif name == 'vlead':
        s['vlead'] = (s['vlead'] + d) % 2
    elif name == 'vel':
        s['vel'] = max(20, min(127, s['vel'] + d * 4))
    elif name == 'dark':
        s['dark'] = max(0, min(100, s['dark'] + d * 5))
    elif name == 'tens':
        s['tens'] = max(0, min(100, s['tens'] + d * 5))
    elif name == 'len':
        s['len'] = LEN_OPTS[(LEN_OPTS.index(s['len']) + d) % 3]

def note_msg(txt, pfx=''):
    global msg, msg_until
    msg = (pfx + ' ' + txt) if pfx else txt
    msg_until = time.monotonic() + 1.5


# =========================
# OLED UI
# =========================

grp = displayio.Group()
lbl_hdr  = label.Label(terminalio.FONT, text="", color=0xFFFFFF, x=0, y=5)
lbl_mid  = label.Label(terminalio.FONT, text="", color=0xFFFFFF, x=0, y=16)
lbl_rows = [label.Label(terminalio.FONT, text="", color=0xFFFFFF, x=0, y=y)
            for y in (30, 39, 48, 57)]
for _l in [lbl_hdr, lbl_mid] + lbl_rows:
    grp.append(_l)
display.root_group = grp

lbl_hdr.text = 'ChordBox'
lbl_mid.text = 'chord discovery'
time.sleep(0.9)

def render_legend():
    lbl_hdr.text = ''.join('%-5s' % x for x in CMD_LBL[0])
    lbl_mid.text = ''.join('%-5s' % x for x in CMD_LBL[1])
    for i in range(4):
        lbl_rows[i].text = ''.join('%-5s' % x for x in GRID_LBL[i])

def render_main(now):
    s = state
    lbl_hdr.text = "%s%+d %-7s%d/%d" % (
        NOTE_NAMES[s['root_pc']], s['octave'], BANKS[s['bank']][0][:7],
        s['page'] + 1, bank_pages(s['bank']))
    if msg and now < msg_until:
        mid = msg
    elif focus != 'page' and now < focus_until:
        mid = ">%s %s" % (LBL.get(focus, focus), val_str(focus))
    else:
        mid = "%-8s%-9s%s" % (last_sym, last_rn[:9],
                              '*' if s['latch'] else '')
    lbl_mid.text = mid
    b = s['bank']
    base = s['page'] * ROWS_PER_PAGE
    def cell(rl):
        nm = row_name(b, base + rl)
        if not nm:
            return ' ' * 10
        return " %d%-8s" % (rl + 1, nm[:7])
    lbl_rows[0].text = cell(0) + cell(2)
    lbl_rows[1].text = cell(1) + cell(3)
    lbl_rows[2].text = ' ' * 10 + cell(4)
    lbl_rows[3].text = ' ' * 10 + cell(5)

def render():
    if shift_down:
        render_legend()
    else:
        render_main(time.monotonic())


# =========================
# KEYPAD SCAN
# =========================

def scan_matrix(rows, cols):
    pressed = []
    for r in rows:
        r.value = True
    for ri, row in enumerate(rows):
        row.value = False
        time.sleep(0.0002)
        for ci, col in enumerate(cols):
            if not col.value:
                pressed.append((ri, ci))
        row.value = True
    return pressed

prev_left  = set()
prev_right = set()

def poll_keypads():
    global prev_left, prev_right
    cur_left  = set(scan_matrix(rows_left, cols_left))
    cur_right = set(scan_matrix(rows_right, cols_right))
    out = (cur_left - prev_left, prev_left - cur_left,
           cur_right - prev_right, prev_right - cur_right)
    prev_left, prev_right = cur_left, cur_right
    return out


# =========================
# CHORD ENGINE
# =========================

def _substitute(roman):
    pool = SUBS.get(roman) or SUBS.get(_base_triad(roman))
    if pool:
        return random.choice(pool)
    return roman

def _add_extension(intervals):
    root = intervals[0]
    return list(intervals) + [root + random.choice([10, 11, 14])]

def _randomize_voicing(notes, r, allow_jump):
    notes = list(notes)
    for i in range(1, len(notes)):
        if random.random() < r * 0.4:
            notes[i] += 12
    if len(notes) > 2 and random.random() < r * 0.3:
        notes[-1] -= 12
    if allow_jump and random.random() < r * 0.25:
        shift = random.choice([-12, 12])
        notes = [n + shift for n in notes]
    return notes

def _voice_lead(notes, prev, anchor):
    cur = sorted(notes)
    best, best_score = cur, 1e9
    n = len(cur)
    for rot in range(n):
        if rot == 0:
            v = cur
        else:
            v = sorted(cur[rot:] + [x + 12 for x in cur[:rot]])
        for sh in (-24, -12, 0, 12, 24):
            cand = [x + sh for x in v]
            if cand[0] < 30 or cand[-1] > 100:
                continue
            mean = sum(cand) / len(cand)
            if abs(mean - anchor) > 6:
                continue
            if prev:
                pv = sorted(prev)
                m = min(len(cand), len(pv))
                score = sum(abs(cand[i] - pv[i]) for i in range(m))
                score += 3 * abs(len(cand) - len(pv))
            else:
                score = 0
            score += 0.35 * abs(mean - anchor)
            if score < best_score:
                best, best_score = cand, score
    return best

def apply_voicing(iv):
    m = state['voic']
    iv = list(iv)
    if m == 0 or len(iv) < 3:
        return iv
    if m == 1:                     # open: drop 2nd-from-top an octave
        iv[-2] -= 12
        return iv
    third = iv[1] - iv[0]
    seventh = (iv[3] - iv[0]) if len(iv) > 3 else (10 if third == 3 else 11)
    if m == 2:                     # shell: 1-3-7
        return [iv[0], iv[0] + third, iv[0] + seventh]
    out = [iv[0] + third, iv[0] + seventh, iv[0] + 14]   # rootless 3-7-9
    if state['bass'] == 0:
        out.insert(0, iv[0])
    return out

def build(roman, mix):
    """Turn a roman numeral into notes. Same pad + same seed = same chord."""
    global last_voicing
    r = state['dice'] / 100.0
    random.seed(session_seed ^ mix)
    if r > 0 and random.random() < r:
        roman = _substitute(roman)
    iv0 = list(CHORDS.get(roman, (0, 4, 7)))
    if r > 0 and random.random() < r * 0.7:
        iv0 = _add_extension(iv0)
    base = 60 + state['octave'] * 12 + state['root_pc']
    root_note = base + iv0[0]
    sym = chord_symbol(state['root_pc'], iv0)
    notes = [base + i for i in apply_voicing(iv0)]
    if r > 0:
        notes = _randomize_voicing(notes, r, allow_jump=not state['vlead'])
    if state['vlead']:
        notes = _voice_lead(notes, last_voicing, base + 5)
    notes = [n for n in notes if 0 <= n <= 127]
    if notes:
        last_voicing = notes
    return notes, root_note, sym, roman

def _vel():
    h = state['hum'] / 100.0
    v = state['vel']
    if h > 0:
        v += int((random.random() - 0.5) * h * 40)
    return max(20, min(127, v))

def play_pad(key, absrow, col):
    """A pad went down. Sounds immediately — nothing is quantised."""
    global last_sym, last_rn, last_row
    if key in active:                      # re-tap while latched = release
        release(key)
        if state['latch']:
            return
    if state['latch'] and active:
        release_all()
    toks = row_tokens(state['bank'], absrow)
    if not toks:
        return
    roman = toks[col % len(toks)]
    ref = row_ref(state['bank'], absrow)
    notes, root, sym, rn = build(roman, ref[0] * 977 + ref[1] * 131 + col * 29)
    if not notes:
        return
    now = time.monotonic()
    random.seed(int(now * 1000000) & 0x7FFFFFFF)   # feel is live, not seeded
    pairs = []
    b = state['bass']
    if b:
        for bn in ((root - 12,) if b == 1 else
                   (root - 24,) if b == 2 else (root - 12, root - 5)):
            if 0 <= bn <= 127:
                pairs.append((bn, midi2))
    for n in sorted(notes):
        pairs.append((n, midi))
    spread = state['sprd'] * 0.00045          # 0 .. 45 ms per note
    h = state['hum'] / 100.0
    for i, (n, m) in enumerate(pairs):
        d = i * spread
        if h:
            d += random.random() * 0.012 * h
        if d < 0.001:
            m.send(NoteOn(n, _vel()))
        else:
            sched.append([now + d, n, _vel(), m, key])
    active[key] = pairs
    last_sym = sym
    last_rn = rn
    last_row = absrow

def release(key):
    global sched
    pairs = active.pop(key, None)
    if not pairs:
        return
    if sched:
        sched = [e for e in sched if e[4] != key]
    for n, m in pairs:
        m.send(NoteOff(n, 0))

def release_all():
    for key in list(active.keys()):
        release(key)

def panic():
    global sched
    sched = []
    active.clear()
    for n in range(128):
        try:
            midi.send(NoteOff(n, 0))
            midi2.send(NoteOff(n, 0))
        except Exception:
            pass


# =========================
# PROGRESSION GENERATOR
# =========================

# Loop shapes that actually work in house / deep house, not random walks.
TEMPL_MIN = (
    ('i', 'bVI', 'bIII', 'bVII'),
    ('i', 'iv', 'bVI', 'v'),
    ('i', 'bVII', 'bVI', 'bVII'),
    ('i', 'bIII', 'iv', 'v'),
    ('iv', 'i', 'bVI', 'bVII'),
    ('i', 'v', 'bVI', 'iv'),
    ('bVI', 'bVII', 'i', 'i'),
    ('i', 'IV', 'i', 'bVII'),        # dorian
    ('i', 'ii', 'bVII', 'i'),        # dorian
    ('ii0', 'v', 'i', 'bVI'),
    ('i', 'bIII', 'bVII', 'iv'),
    ('iv', 'bVII', 'i', 'i'),
)
TEMPL_MAJ = (
    ('I', 'vi', 'IV', 'V'),
    ('vi', 'IV', 'I', 'V'),
    ('I', 'iii', 'vi', 'IV'),
    ('ii', 'V', 'I', 'I'),
    ('IV', 'V', 'iii', 'vi'),
    ('I', 'IV', 'vi', 'V'),
    ('IV', 'I', 'ii', 'vi'),
    ('I', 'bVII', 'IV', 'I'),        # mixolydian
    ('I', 'iv', 'I', 'V'),           # modal mixture
    ('vi', 'ii', 'V', 'I'),
    ('IV', 'iii', 'ii', 'I'),
    ('I', 'V', 'vi', 'iii'),
)

def _vary(block):
    """Same shape, one chord swapped for a relative — usually the turnaround."""
    out = list(block)
    n = len(out)
    start = n - 1 if random.random() < 0.6 else random.randrange(n)
    for k in range(n):
        i = (start + k) % n
        pool = SUBS.get(out[i]) or SUBS.get(_base_triad(out[i]))
        if pool:
            out[i] = random.choice(pool)
            break
    return out

def gen_prog():
    dark = state['dark'] / 100.0
    tens = state['tens'] / 100.0
    random.seed(session_seed ^ (gen_count * 7717) ^ 0x517)
    pool = TEMPL_MIN if random.random() < dark else TEMPL_MAJ
    head = list(random.choice(pool))
    toks = list(head)
    while len(toks) < state['len']:
        if random.random() < 0.55:
            toks += _vary(head)
        else:
            toks += list(random.choice(pool))
    toks = toks[:state['len']]
    for i, t in enumerate(toks):
        if random.random() < tens:
            for suf in ('9', 'maj7', '7', 'add9'):
                if t + suf in CHORDS:
                    toks[i] = t + suf
                    break
    return toks

def _install(toks, name, bank_name):
    """Add a token list to PROGS + a bank, 4 chords per pad row."""
    rows = []
    gi = 0
    part = 0
    while gi < len(toks):
        g = toks[gi:gi + 8]
        nm = name if part == 0 else name + chr(96 + part)
        if len(g) > 4:
            nm = nm + '|' + nm + "'"
        idx = len(PROGS)
        PROGS.append((nm, ' '.join(g)))
        rows.append(idx * 2)
        if len(g) > 4:
            rows.append(idx * 2 + 1)
        gi += 8
        part += 1
    bi = None
    for i in range(len(BANKS)):
        if BANKS[i][0] == bank_name:
            bi = i
            break
    if bi is None:
        BANKS.append((bank_name, rows))
        bi = len(BANKS) - 1
        first = 0
    else:
        first = len(BANKS[bi][1])
        BANKS[bi][1].extend(rows)
    return bi, first // ROWS_PER_PAGE


# =========================
# COMMANDS
# =========================

def cmd_gen():
    global gen_count
    gen_count += 1
    toks = gen_prog()
    bi, pg = _install(toks, 'G%02d' % gen_count, 'New')
    state['bank'] = bi
    state['page'] = pg
    note_msg('G%02d' % gen_count, 'NEW')

def cmd_star():
    global fav_count
    toks = row_tokens(state['bank'], last_row)
    if not toks:
        note_msg('n/a')
        return
    fav_count += 1
    name = 'F%02d' % fav_count
    ok = True
    try:
        with open('favs.txt', 'a') as f:
            f.write('P\t%s\t%s\n' % (name, ' '.join(toks)))
    except OSError:
        ok = False
    _install(toks, name, 'Favs')
    note_msg(name if ok else name + ' RO!', 'FAV')

def cmd_plain():
    state['dice'] = 0
    state['hum'] = 0
    state['sprd'] = 0
    state['voic'] = 0
    state['bass'] = 0
    note_msg('PLAIN')

def do_cmd(n):
    if not n:
        return
    if n == 'star':
        cmd_star()
    elif n == 'gen':
        cmd_gen()
    elif n == 'ltch':
        state['latch'] = not state['latch']
        if not state['latch']:
            release_all()
        note_msg('latch ' + ('on' if state['latch'] else 'off'))
    elif n == 'panc':
        panic()
        note_msg('PANIC')
    elif n == 'plain':
        cmd_plain()

def do_reroll(now_):
    global session_seed, last_voicing
    session_seed = int(now_ * 1000) & 0x7FFF
    last_voicing = None
    note_msg('rr%02X' % (session_seed & 0xFF))

_btn_states = {}

def _debounce(btn_obj, name):
    cur = not btn_obj.value
    prev = _btn_states.get(name, False)
    _btn_states[name] = cur
    if cur and not prev:
        return 'press'
    if not cur and prev:
        return 'release'
    return None


# =========================
# MAIN LOOP
# =========================

render()

while True:
    now = time.monotonic()

    # --- strum tails: wall-clock only, never a MIDI clock ---
    if sched:
        keep = []
        for e in sched:
            if now >= e[0]:
                e[3].send(NoteOn(e[1], e[2]))
            else:
                keep.append(e)
        sched = keep

    # --- drain MIDI in and discard it (clock must not affect playback) ---
    if _in_port:
        _in_port.read(64)

    # --- SHIFT ---
    if _debounce(btn_shift, 'shift'):
        ui_dirty = True
    shift_down = _btn_states.get('shift', False)

    # --- encoder: turn = focused param (home = PAGE), push+turn = BANK ---
    if focus != 'page' and now >= focus_until:
        focus = 'page'
        ui_dirty = True
    enc_down = not enc_btn.value
    enc_pos = encoder.position
    if enc_pos != last_enc_pos:
        d = enc_pos - last_enc_pos
        last_enc_pos = enc_pos
        if enc_down:
            enc_moved = True
            adjust('bank', d)
        else:
            adjust(focus, d)
            if focus != 'page':
                focus_until = now + FOCUS_HOLD
        ui_dirty = True
    ev = _debounce(enc_btn, 'enc')
    if ev == 'press':
        enc_moved = False
    elif ev == 'release':
        if not enc_moved:
            focus = VITALS[(VITALS.index(focus) + 1) % len(VITALS)] \
                if focus in VITALS else VITALS[0]
            focus_until = now + FOCUS_HOLD
        ui_dirty = True

    # --- CONFIRM: tap = reroll, hold = latch, shift = star ---
    ev = _debounce(btn_conf, 'conf')
    if ev == 'press':
        conf_press_t = now
        conf_shift = shift_down
    elif ev == 'release' and conf_press_t is not None:
        if conf_shift or shift_down:
            cmd_star()
        elif now - conf_press_t >= 0.7:
            state['latch'] = not state['latch']
            if not state['latch']:
                release_all()
            note_msg('latch ' + ('on' if state['latch'] else 'off'))
        else:
            do_reroll(now)
        conf_press_t = None
        ui_dirty = True

    # --- BACK: tap = all notes off, hold = PLAIN ---
    ev = _debounce(btn_back, 'back')
    if ev == 'press':
        back_press_t = now
    elif ev == 'release' and back_press_t is not None:
        if now - back_press_t >= 0.7:
            cmd_plain()
        else:
            panic()
            state['latch'] = False
            last_voicing = None
        back_press_t = None
        ui_dirty = True

    # --- keypads ---
    new_left, rel_left, new_right, rel_right = poll_keypads()

    for (r, c) in rel_left:
        if not state['latch']:
            release(('L', r, c))
    for (r, c) in rel_right:
        if not state['latch']:
            release(('R', r, c))

    if shift_down:
        for (r, c) in new_right:                  # focus a parameter
            n = GRID[r][c]
            if n:
                focus = n
                focus_until = now + FOCUS_HOLD
                ui_dirty = True
        for (r, c) in new_left:                   # commands (rows 1+4)
            if r == 0:
                do_cmd(CMDS[0][c])
                ui_dirty = True
            elif r == 3:
                do_cmd(CMDS[1][c])
                ui_dirty = True
    else:
        base = state['page'] * ROWS_PER_PAGE
        for (r, c) in new_left:
            if r == 0:
                play_pad(('L', r, c), base + 0, c)
                ui_dirty = True
            elif r == 3:
                play_pad(('L', r, c), base + 1, c)
                ui_dirty = True
        for (r, c) in new_right:
            play_pad(('R', r, c), base + 2 + r, c)
            ui_dirty = True

    # --- redraw when a message / focus flash expires ---
    special = (msg and now < msg_until) or (focus != 'page' and now < focus_until)
    if special != _was_special:
        _was_special = special
        ui_dirty = True

    if ui_dirty:
        render()
        ui_dirty = False

    time.sleep(0.001 if sched else 0.003)
