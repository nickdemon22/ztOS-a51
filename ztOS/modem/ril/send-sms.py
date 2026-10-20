# Send one SMS through the modem's AT port in PDU mode.
import os, select, sys, time
fd = os.open("/dev/umts_router", os.O_RDWR | os.O_NONBLOCK)
keep = [os.open("/dev/umts_ipc0", os.O_RDWR | os.O_NONBLOCK), os.open("/dev/umts_rfs0", os.O_RDWR | os.O_NONBLOCK)]
buf = b""
def wait_for(tokens, secs):
    global buf
    end = time.time() + secs
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.2)
        if r:
            data = os.read(fd, 4096); buf += data
            print("<- %r" % data, flush=True)
            for t in tokens:
                if t in buf:
                    buf = b""; return t
    return None
def at(cmd, tokens=(b"OK", b"ERROR"), secs=5):
    os.write(fd, (cmd + "\r").encode()); print("-> %s" % cmd, flush=True)
    return wait_for(tokens, secs)
def bcd(digits):
    if len(digits) % 2: digits += "F"
    return bytes(int(digits[i + 1] + digits[i], 16) for i in range(0, len(digits), 2))
def gsm7(text):
    bits, n, out = 0, 0, bytearray()
    for ch in text:
        bits |= (ord(ch) & 0x7f) << n; n += 7
        while n >= 8: out.append(bits & 0xff); bits >>= 8; n -= 8
    if n: out.append(bits & 0xff)
    return bytes(out)
number, text = sys.argv[1].lstrip("+"), sys.argv[2]
tpdu = bytes([0x11, 0x00, len(number), 0x91]) + bcd(number) + bytes([0x00, 0x00, 0xA7, len(text)]) + gsm7(text)
at("AT+CMEE=2")
at("AT+CMGF=0")
if at("AT+CMGS=%d" % len(tpdu), (b">", b"ERROR"), 10) == b">":
    os.write(fd, ("00" + tpdu.hex().upper()).encode() + b"\x1a")
    print("-> 00%s (ctrl-z)" % tpdu.hex().upper(), flush=True)
    wait_for((b"+CMGS", b"ERROR"), 60)
    wait_for((b"OK",), 5)
