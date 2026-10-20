#!/usr/bin/env python3
# dsim.py: send Samsung's S6E3HA8 (starlte) panel sequences straight to the DSIM, from /dev/mem.
# The command bytes come out of the stock kernel's panel tables (star_a3_s0_*): the sleep-out
# ("init-seq") re-sends DSC, PPS, scaler, area, TE and the rest, and the access keys wrap them.
# Run as root with the screen off.
#   dsim.py down      display off, sleep in   (Samsung's display-off-seq and exit-seq)
#   dsim.py up        sleep out, re-init, display on   (then "screen on" from the shell)
#   dsim.py resume    only the re-init, for after a bare sleep out
import mmap
import os
import struct
import sys
import time

PKTHDR, PAYLOAD, INTSRC = 0x58, 0x5C, 0x50
PH_EMPTY = 1 << 28
fd = os.open("/dev/mem", os.O_RDWR | os.O_SYNC)
m = mmap.mmap(fd, 0x1000, offset=0x16080000)


def rd(o):
    return struct.unpack_from("<I", m, o)[0]


def wr(o, v):
    struct.pack_into("<I", m, o, v)


# DECON0 and its hardware-trigger control: in command mode a frame transfer starts on
# every TE edge while the trigger is unmasked, and it shares the command FIFO, so a
# panel command sent in the middle of one is dropped. Samsung masks the trigger around
# its panel commands; so do we. If this script dies with the trigger masked, ztMETRO's
# next frame unmasks it again (the driver does that on every flip).
dec = mmap.mmap(os.open("/dev/mem", os.O_RDWR | os.O_SYNC), 0x1000, offset=0x16030000)
TRIG, TRIG_EN, TRIG_MASK = 0x70, 1 << 0, 1 << 4


def trigger(on):
    v = struct.unpack_from("<I", dec, TRIG)[0]
    v = (v | TRIG_EN) & ~TRIG_MASK if on else (v | TRIG_EN | TRIG_MASK)
    struct.pack_into("<I", dec, TRIG, v)
    time.sleep(0.02)                              # let the frame in flight finish


def fifo_idle():
    """Samsung's dsim_reg_header_fifo_is_empty: FIFO status bit 10."""
    for _ in range(5000):
        if rd(0x68) & 0x400:
            return True
        time.sleep(0.00002)
    return False


def send(dt, data=b""):
    fifo_idle()
    _send(dt, data)
    fifo_idle()
    time.sleep(0.001)


def _send(dt, data=b""):
    wr(INTSRC, PH_EMPTY)
    if dt in (0x05, 0x15, 0x07):                      # short packets
        d = data + b"\0\0"
        wr(PKTHDR, dt | d[0] << 8 | d[1] << 16)
    else:                                             # long: the payload words first, then the header
        for i in range(0, len(data), 4):
            wr(PAYLOAD, int.from_bytes(data[i:i + 4].ljust(4, b"\0"), "little"))
        wr(PKTHDR, dt | (len(data) & 0xFF) << 8 | (len(data) >> 8) << 16)
    for _ in range(2000):
        if rd(INTSRC) & PH_EMPTY:
            return
        time.sleep(0.00001)
    print("panel did not take", hex(dt), data[:4].hex())


def cmd(*b):                                          # a DCS write: short or long as its length says
    send(0x05 if len(b) == 1 else 0x15 if len(b) == 2 else 0x39, bytes(b))


def wait(us):
    time.sleep(us / 1e6)


L1, L1X = (0x9F, 0xA5, 0xA5), (0x9F, 0x5A, 0x5A)
L2, L2X = (0xF0, 0x5A, 0x5A), (0xF0, 0xA5, 0xA5)
L3, L3X = (0xFC, 0x5A, 0x5A), (0xFC, 0xA5, 0xA5)
# The 1440x2960 row of star_a3_s0_pps_table (the packet's own bytes are the 1080x2220 template:
# sending those gives static). Slices are 720x40.
PPS = bytes.fromhex(
    "1100008930800b9005a0002802d002d0020002680020046c000a000c027701e9180010f0030c2000060b0b330e1c2a3846"
    "5462697077797b7d7e01020100094009be19fc19fa19f81a381a781ab62af62b342b743b746bf4" + "00" * 49)


PPS = PPS[:128].ljust(128, b"\0")                     # (always 128 bytes)


def down():                                           # display-off-seq, then exit-seq
    cmd(*L1); cmd(0x28); cmd(*L1X)
    wait(20000)
    cmd(*L2); cmd(0xE2, 0x00); cmd(*L2X)              # afc-off-seq
    wait(20000)
    cmd(*L1); cmd(0x10)                               # sleep in (the key stays enabled, as in exit-seq)
    wait(100000)


def reinit():                                         # the init-seq after the sleep out
    cmd(*L2); cmd(0xBA, 0x01, 0x26, 0x08, 0x08, 0xF3); cmd(*L2X)           # scaler
    cmd(0x2A, 0x00, 0x00, 0x05, 0x9F); cmd(0x2B, 0x00, 0x00, 0x0B, 0x8F)   # caset, paset (1440x2960 rows)
    cmd(*L1); cmd(0x1A, 0x1F, 0x00, 0x00, 0x00, 0x00); cmd(0x35, 0x00); cmd(*L1X)   # set_area, te_on
    cmd(*L2); cmd(0xED, 0x4C); cmd(*L3)                                    # err_fg
    cmd(0xC5, 0x0D, 0x10, 0xB4, 0x3E, 0x01); cmd(*L3X)                     # ffc
    cmd(0xB9, 0x01, 0xB0, 0x81, 0x09, 0x00, 0x00, 0x00, 0x11, 0x03)        # tsp_hsync
    cmd(*L2X)                                         # (isc F6 43 sits at register offset 3: not sent)


def up():
    wait(5000)
    cmd(*L1); send(0x07, b"\x01"); send(0x0A, PPS); cmd(*L1X)             # DSC on, PPS
    cmd(0x11)                                                              # sleep out
    wait(10000)
    reinit()
    wait(120000)
    cmd(*L1); cmd(0x29); cmd(*L1X)                                         # display-on-seq


RXFIFO, FIFOSTAT = 0x60, 0x68
RX_DONE, RX_EMPTY = 1 << 18, 1 << 12


def read(reg, n):
    """A DCS read as Samsung's dsim_read_data does it: set the return size, ask with a
    bus turn-around, then take the answer out of the receive FIFO. None if the panel
    did not answer."""
    while not rd(FIFOSTAT) & RX_EMPTY:                # anything left over
        rd(RXFIFO)
    wr(INTSRC, RX_DONE | PH_EMPTY)
    wr(PKTHDR, 0x37 | (n & 0xFF) << 8)                # set maximum return packet size
    for _ in range(2000):
        if rd(INTSRC) & PH_EMPTY:
            break
        time.sleep(0.00001)
    wr(INTSRC, PH_EMPTY)
    wr(PKTHDR, 0x06 | reg << 8 | 1 << 24)             # DCS read, with the turn-around
    for _ in range(300):
        if rd(INTSRC) & RX_DONE:
            break
        time.sleep(0.001)
    else:
        return None
    h = rd(RXFIFO)
    t = h & 0xFF
    if t in (0x1C, 0x1A):                             # long answer: length, then the bytes
        ln = h >> 8 & 0xFFFF
        out = b""
        for _ in range((ln + 3) // 4):
            out += rd(RXFIFO).to_bytes(4, "little")
        out = out[:ln]
    elif t in (0x11, 0x21):
        out = bytes([h >> 8 & 0xFF])
    elif t in (0x12, 0x22):
        out = bytes([h >> 8 & 0xFF, h >> 16 & 0xFF])
    else:
        out = None                                    # 0x02: the panel's error report
    while not rd(FIFOSTAT) & RX_EMPTY:
        rd(RXFIFO)
    return out


def dump():
    """Every readable register, for comparing the panel before and after a sleep."""
    for attempt in range(10):                         # the keys must have taken, or most registers read as 0
        cmd(*L1); cmd(*L2); cmd(*L3)
        time.sleep(0.02)
        k = read(0xF0, 2)
        k3 = read(0xFC, 2)
        if k == b"\x5a\x5a" and k3 == b"\x5a\x5a":
            break
        time.sleep(0.05)
    print("# unlock after %d tries: f0=%s fc=%s" % (attempt + 1, k.hex() if k else "-", k3.hex() if k3 else "-"))
    for reg in list(range(0x04, 0x10)) + [0x1A, 0x2A, 0x2B, 0x35, 0x51, 0x53, 0x55, 0x9D, 0x9E] + list(range(0xA0, 0x100)):
        for _ in range(4):                            # a read sometimes gets no answer: ask again
            v = read(reg, 60)
            if v is not None and len(v) == 60:
                break
            time.sleep(0.02)
        print("%02x %s" % (reg, v.hex() if v is not None else "-"))
    cmd(*L3X); cmd(*L2X); cmd(*L1X)


def deep():
    """dsim.py deep REG [REG ...]: read a register 240 bytes deep, 60 at a time, with the
    panel's offset register (B0)."""
    for _ in range(10):
        cmd(*L1); cmd(*L2); cmd(*L3)
        time.sleep(0.02)
        if read(0xF0, 2) == b"\x5a\x5a" and read(0xFC, 2) == b"\x5a\x5a":
            break
    for h in sys.argv[2:]:
        reg = int(h, 16)
        out = b""
        for off in range(0, 240, 60):
            for _ in range(4):
                cmd(0xB0, off)
                v = read(reg, 60)
                if v is not None and len(v) == 60:
                    break
                time.sleep(0.02)
            out += v if v else b"?" * 60
        cmd(0xB0, 0)
        print("%02x %s" % (reg, out.hex()))
    cmd(*L3X); cmd(*L2X); cmd(*L1X)


def peek():
    """dsim.py r REG [REG ...]: the first 16 bytes of each register."""
    for _ in range(10):
        cmd(*L1); cmd(*L2); cmd(*L3)
        time.sleep(0.02)
        if read(0xF0, 2) == b"\x5a\x5a" and read(0xFC, 2) == b"\x5a\x5a":
            break
    for h in sys.argv[2:]:
        v = None
        for _ in range(4):
            v = read(int(h, 16), 16)
            if v:
                break
        print("%s %s" % (h, v.hex() if v else "-"))
    cmd(*L3X); cmd(*L2X); cmd(*L1X)


# star_a3_s0_aor_table, 573 levels: 0 dimmest, 572 brightest (register B1, 2 bytes).
AOR_TABLE = (
    0x0b76, 0x0b75, 0x0b73, 0x0b71, 0x0b6f, 0x0b6d, 0x0b63, 0x0b62, 0x0b5f, 0x0b5d, 0x0b53,
    0x0b51, 0x0b4f, 0x0b4d, 0x0b44, 0x0b43, 0x0b41, 0x0b3f, 0x0b3d, 0x0b33, 0x0b31, 0x0b30,
    0x0b2d, 0x0b23, 0x0b21, 0x0b1f, 0x0b1d, 0x0b13, 0x0b12, 0x0b0f, 0x0b0d, 0x0b03, 0x0b01,
    0x0aff, 0x0afd, 0x0af7, 0x0af3, 0x0af0, 0x0aec, 0x0ae8, 0x0ae4, 0x0ae0, 0x0adc, 0x0ad8,
    0x0ad3, 0x0acf, 0x0acb, 0x0ac6, 0x0ac2, 0x0abe, 0x0ab9, 0x0ab5, 0x0ab0, 0x0aac, 0x0aa7,
    0x0aa2, 0x0a9e, 0x0a98, 0x0a92, 0x0a8c, 0x0a86, 0x0a80, 0x0a7a, 0x0a73, 0x0a6d, 0x0a66,
    0x0a60, 0x0a59, 0x0a52, 0x0a4c, 0x0a46, 0x0a40, 0x0a3a, 0x0a34, 0x0a2e, 0x0a26, 0x0a1f,
    0x0a18, 0x0a10, 0x0a09, 0x0a01, 0x09f9, 0x09f1, 0x09ea, 0x09e2, 0x09da, 0x09d2, 0x09cd,
    0x09c8, 0x09c2, 0x09bd, 0x09b8, 0x09b2, 0x09ad, 0x09a7, 0x09a2, 0x099d, 0x0998, 0x0992,
    0x098c, 0x0987, 0x0982, 0x097d, 0x0978, 0x0972, 0x096d, 0x0967, 0x0962, 0x095c, 0x0957,
    0x0952, 0x094d, 0x0948, 0x0943, 0x093e, 0x0939, 0x0934, 0x092f, 0x092a, 0x0925, 0x0920,
    0x091b, 0x0915, 0x090b, 0x0902, 0x08f8, 0x08ee, 0x08e4, 0x08db, 0x08d2, 0x08cd, 0x08c8,
    0x08c3, 0x08be, 0x08b9, 0x08b4, 0x08aa, 0x08a1, 0x0897, 0x088e, 0x0884, 0x087b, 0x0872,
    0x086d, 0x0868, 0x0863, 0x085d, 0x0858, 0x0853, 0x0849, 0x083f, 0x0836, 0x082c, 0x0823,
    0x0819, 0x080e, 0x0804, 0x07fb, 0x07f2, 0x07e8, 0x07df, 0x07d5, 0x07cd, 0x07c4, 0x07ba,
    0x07b1, 0x07a7, 0x079e, 0x0794, 0x078c, 0x0782, 0x0778, 0x076e, 0x0764, 0x075a, 0x0750,
    0x0745, 0x073a, 0x072f, 0x0724, 0x071a, 0x070e, 0x0701, 0x06f4, 0x06e7, 0x06db, 0x06ce,
    0x06c0, 0x06b4, 0x06a8, 0x069b, 0x068f, 0x0683, 0x0677, 0x066c, 0x065f, 0x0652, 0x0645,
    0x0638, 0x062b, 0x061e, 0x0612, 0x0601, 0x05f0, 0x05e0, 0x05cf, 0x05be, 0x05ad, 0x059e,
    0x0591, 0x0584, 0x0577, 0x056a, 0x055d, 0x0550, 0x0544, 0x0533, 0x0522, 0x0511, 0x0501,
    0x04f0, 0x04df, 0x04d0, 0x04d3, 0x04c5, 0x04b7, 0x04a9, 0x049b, 0x048d, 0x0480, 0x04e7,
    0x04d5, 0x04c4, 0x04b3, 0x04a1, 0x0490, 0x0480, 0x04e1, 0x04d0, 0x04c0, 0x04af, 0x049f,
    0x048f, 0x0480, 0x04db, 0x04cc, 0x04bc, 0x04ad, 0x049d, 0x048e, 0x0480, 0x04d8, 0x04cb,
    0x04bf, 0x04b2, 0x04a6, 0x0499, 0x048d, 0x0480, 0x04e7, 0x04d8, 0x04ca, 0x04bb, 0x04ac,
    0x049d, 0x048f, 0x0480, 0x04e1, 0x04d3, 0x04c5, 0x04b7, 0x04aa, 0x049c, 0x048e, 0x0480,
    0x04db, 0x04ce, 0x04c1, 0x04b4, 0x04a7, 0x049a, 0x048d, 0x0480, 0x04e6, 0x04d8, 0x04c9,
    0x04bb, 0x04ac, 0x049d, 0x048f, 0x0480, 0x04e2, 0x04d6, 0x04ca, 0x04be, 0x04b2, 0x04a5,
    0x0499, 0x048d, 0x0480, 0x04dc, 0x04d1, 0x04c6, 0x04ba, 0x04af, 0x04a3, 0x0498, 0x048d,
    0x0480, 0x04e5, 0x04d9, 0x04cc, 0x04c0, 0x04b3, 0x04a7, 0x049a, 0x048e, 0x0480, 0x04de,
    0x04d0, 0x04c3, 0x04b6, 0x04a8, 0x049b, 0x048d, 0x0480, 0x04e6, 0x04d9, 0x04cd, 0x04c0,
    0x04b4, 0x04a7, 0x049a, 0x048e, 0x0480, 0x04e1, 0x04d6, 0x04cc, 0x04c1, 0x04b6, 0x04ab,
    0x04a0, 0x0496, 0x048b, 0x0480, 0x04e5, 0x04d9, 0x04cc, 0x04c0, 0x04b3, 0x04a7, 0x049a,
    0x048e, 0x0480, 0x04e0, 0x04d6, 0x04cb, 0x04c0, 0x04b6, 0x04ab, 0x04a0, 0x0496, 0x048b,
    0x0480, 0x04e5, 0x04d9, 0x04ce, 0x04c3, 0x04b8, 0x04ad, 0x04a2, 0x0496, 0x048b, 0x0480,
    0x04de, 0x04d4, 0x04cb, 0x04c1, 0x04b8, 0x04af, 0x04a5, 0x049c, 0x0492, 0x0489, 0x047e,
    0x046c, 0x0459, 0x0446, 0x0434, 0x0421, 0x040e, 0x03fc, 0x03e9, 0x03d7, 0x03c4, 0x03b2,
    0x039f, 0x038d, 0x037a, 0x0365, 0x034f, 0x033a, 0x0324, 0x030f, 0x02fa, 0x02e4, 0x02ce,
    0x02b7, 0x02a1, 0x028a, 0x0274, 0x025d, 0x0246, 0x0230, 0x0202, 0x01ef, 0x01dc, 0x01c9,
    0x01b6, 0x01a2, 0x018f, 0x017c, 0x020e, 0x01fc, 0x01ea, 0x01d7, 0x01c5, 0x01b3, 0x01a0,
    0x018e, 0x017c, 0x0206, 0x01f4, 0x01e3, 0x01d2, 0x01c1, 0x01af, 0x019e, 0x018d, 0x017c,
    0x0207, 0x01f8, 0x01e8, 0x01d9, 0x01c9, 0x01ba, 0x01aa, 0x019b, 0x018b, 0x017c, 0x01ed,
    0x01dd, 0x01cd, 0x01bd, 0x01ad, 0x019c, 0x018c, 0x017c, 0x01f1, 0x01e2, 0x01d3, 0x01c5,
    0x01b6, 0x01a8, 0x0199, 0x018a, 0x017c, 0x01eb, 0x01dd, 0x01cf, 0x01c1, 0x01b3, 0x01a6,
    0x0198, 0x018a, 0x017c, 0x01a4, 0x0197, 0x0189, 0x017c, 0x01a1, 0x0194, 0x0187, 0x017a,
    0x0165, 0x0150, 0x013b, 0x012c, 0x011d, 0x010e, 0x00ff, 0x00eb, 0x00d7, 0x00c3, 0x00af,
    0x0091, 0x0073, 0x0055, 0x0037, 0x003a, 0x002b, 0x001b, 0x000c, 0x0039, 0x002a, 0x001b,
    0x000c, 0x0039, 0x002a, 0x001b, 0x000c, 0x000c, 0x000c, 0x000c, 0x000c, 0x000c, 0x000c,
    0x000c, 0x000c, 0x000c, 0x000c, 0x000c, 0x000c, 0x000c, 0x000c, 0x000c, 0x000c, 0x000c,
    0x000c, 0x000c, 0x000c, 0x000c, 0x000c, 0x000c, 0x000c, 0x000c, 0x000c, 0x000c, 0x000c,
    0x000c, 0x000c, 0x000c, 0x000c, 0x000c, 0x000c, 0x000c, 0x000c, 0x000c, 0x000c, 0x000c,
    0x000c, 0x000c, 0x000c, 0x000c, 0x000c, 0x000c, 0x000c, 0x000c, 0x000c, 0x000c, 0x000c,
    0x000c, 0x000c, 0x000c, 0x000c, 0x000c, 0x000c, 0x000c, 0x000c, 0x000c, 0x000c, 0x000c,
    0x000c)


def aor(level):
    """The panel's own dimming (AOR, register B1), from Samsung's star_a3_s0_aor_table:
    level 0 is the dimmest, 572 the brightest. Sent with the trigger masked."""
    v = AOR_TABLE[max(0, min(572, level))]
    trigger(False)
    try:
        cmd(*L2)
        cmd(0xB1, v >> 8, v & 0xFF)
        cmd(0xF7, 0x03)                           # latch it
        cmd(*L2X)
    finally:
        trigger(True)


def bl():
    """dsim.py bl LEVEL [LEVEL ...]: set the panel's brightness, 3 s apart."""
    for a in sys.argv[2:]:
        aor(int(a))
        time.sleep(3)


def blind():
    """dsim.py wo HEX [HEX ...]: write registers with no reads at all (a read disturbs the
    panel: it clears the first byte of some registers). Keys and writes are sent three times."""
    for _ in range(3):
        cmd(*L1); cmd(*L2); cmd(*L3)
        time.sleep(0.03)
    for _ in range(3):
        for h in sys.argv[2:]:
            cmd(*bytes.fromhex(h))
        time.sleep(0.03)
    cmd(*L3X); cmd(*L2X); cmd(*L1X)


def poke():
    """dsim.py w HEX [HEX ...]: write registers (first byte is the register) under the level 2 and 3 keys."""
    for _ in range(10):
        cmd(*L2); cmd(*L3)
        time.sleep(0.02)
        if read(0xF0, 2) == b"\x5a\x5a":
            break
    for h in sys.argv[2:]:
        cmd(*bytes.fromhex(h))
    for h in sys.argv[2:]:
        b = bytes.fromhex(h)
        print("%02x now %s" % (b[0], (read(b[0], len(b) - 1) or b"").hex()))
    cmd(*L3X); cmd(*L2X)


def up_min():                                   # only the keyed sleep out and display on
    cmd(*L1); cmd(0x11); cmd(*L1X)
    wait(120000)
    cmd(*L1); cmd(0x29); cmd(*L1X)


def up_reinit():                                      # keyed sleep out, the re-init, display on; no DSC/PPS
    cmd(*L1); cmd(0x11); cmd(*L1X)
    wait(10000)
    reinit()
    wait(120000)
    cmd(*L1); cmd(0x29); cmd(*L1X)


def set_bl():                                         # set-bl-seq with the table's default values
    cmd(*L2)
    # (gamma CA and IRC B8 are placeholders in the kernel, filled from the panel's own calibration: not sent)
    cmd(0xB1, 0x00, 0x0C)                                                  # aor
    cmd(0xB5, 0x19, 0xDC, 0x0A); cmd(0xB5, 0x0C)                           # tset_mps_elvss, elvss_temp
    cmd(0xF4, 0xEB, 0x17)                                                  # vgh_vint
    cmd(0xB4, 0x00, 0x44, 0x80, 0x65, 0x26, 0x00); cmd(0xB4, 0x20)         # acl control, dim frame
    cmd(0x55, 0x00)                                                        # acl off
    cmd(0xF7, 0x03)                                                        # gamma_update_enable
    cmd(*L2X)


def up_bl():                                          # keyed wake, re-init, brightness block, display on
    cmd(*L1); cmd(0x11); cmd(*L1X)
    wait(10000)
    reinit()
    set_bl()
    wait(120000)
    cmd(*L1); cmd(0x29); cmd(*L1X)


{"down": down, "up": up, "resume": reinit, "up_min": up_min, "up_reinit": up_reinit, "up_bl": up_bl,
 "dump": dump, "w": poke, "deep": deep, "r": peek, "wo": blind,
 "bl": bl}[sys.argv[1]]()
