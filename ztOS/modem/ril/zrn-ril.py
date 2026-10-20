#!/usr/bin/env python3
"""zrn-ril: talk to the S9's modem in Samsung's IPC on /dev/umts_ipc0.

A prototype: it keeps the command and file channels open (the modem waits for
both before it starts its radio), runs the steps given on the command line and
prints every message, decoded where it is known.

A message is: length (u16, header included), message and ack sequence numbers,
main and sub command, type, data. Requests: 1 exec, 2 get, 3 set, 4 confirm,
5 event. Replies: 1 indication, 2 response, 3 notification.
"""
import os, select, struct, sys, time

NAMES = {
    0x0101: "PWR_PHONE_PWR_UP", 0x0107: "PWR_PHONE_STATE", 0x0104: "PWR_BATT_STATUS",
    0x0401: "SMS_SEND_MSG", 0x0402: "SMS_INCOMING_MSG", 0x0403: "SMS_READ_MSG",
    0x0404: "SMS_SAVE_MSG", 0x0405: "SMS_DEL_MSG", 0x0406: "SMS_DELIVER_REPORT",
    0x0407: "SMS_DEVICE_READY", 0x0408: "SMS_SVC_CENTER_ADDR",
    0x0501: "SEC_SIM_STATUS", 0x0502: "SEC_PHONE_LOCK", 0x0507: "SEC_SIM_ICC_TYPE",
    0x0508: "SEC_LOCK_INFO", 0x0509: "SEC_ISIM_AUTH",
    0x0701: "DISP_ICON_INFO", 0x0706: "DISP_RSSI_INFO",
    0x0801: "NET_PREF_PLMN", 0x0802: "NET_PLMN_SEL", 0x0803: "NET_SERVING_NETWORK",
    0x0804: "NET_PLMN_LIST", 0x0805: "NET_REGIST", 0x0806: "NET_SUBSCRIBER_NUM",
    0x080a: "NET_MODE_SEL", 0x0a01: "MISC_ME_VERSION", 0x0a02: "MISC_ME_IMSI",
    0x0a07: "MISC_TIME_INFO", 0x8001: "GEN_PHONE_RES",
}
SIM = {0x00: "ready", 0x01: "sim lock required", 0x03: "PIN required", 0x04: "PUK required",
       0x80: "no card", 0x81: "card error", 0x82: "initialising done", 0x83: "phonebook ready"}

ipc = os.open("/dev/umts_ipc0", os.O_RDWR | os.O_NONBLOCK)
rfs = os.open("/dev/umts_rfs0", os.O_RDWR | os.O_NONBLOCK)
seq = 0
log = open(sys.argv[1] if len(sys.argv) > 1 and sys.argv[1].startswith("/") else "/dev/null", "a")

def out(text):
    line = "%8.3f %s" % (time.monotonic() % 100000, text)
    print(line, flush=True)

def send(cmd, typ, data=b""):
    global seq
    seq = seq % 254 + 1
    msg = struct.pack("<HBBBBB", 7 + len(data), seq, 0xff, cmd >> 8, cmd & 0xff, typ) + data
    os.write(ipc, msg)
    out("-> %04x %s t%d %s" % (cmd, NAMES.get(cmd, "?"), typ, data.hex()))
    return seq

def decode(cmd, typ, data):
    if cmd == 0x0501 and data:
        return "SIM: %s (0x%02x)" % (SIM.get(data[0], "?"), data[0])
    if cmd == 0x0803 and data:
        return "serving: %r" % data
    if cmd == 0x8001 and len(data) >= 5:
        g, i, t, code = data[0], data[1], data[2], struct.unpack("<H", data[3:5])[0]
        return "result for %02x%02x t%d: 0x%04x%s" % (g, i, t, code, " (ok)" if code == 0x8000 else "")
    if cmd == 0x0805 and len(data) >= 3:
        state = {1: "not registered", 2: "registered, home", 3: "searching", 4: "denied",
                 5: "unknown", 6: "registered, roaming"}.get(data[2], "?")
        return "act 0x%02x domain %d: %s" % (data[0], data[1], state)
    if cmd == 0x0401 and data:
        return "send result: %s" % data.hex()
    return ""

def pump(secs):
    end = time.time() + secs
    while time.time() < end:
        r, _, _ = select.select([ipc, rfs], [], [], 0.2)
        for fd in r:
            try:
                data = os.read(fd, 65536)
            except OSError as e:
                out("read error %s" % e); continue
            if fd == rfs:
                out("<- RFS %d: %s" % (len(data), data[:64].hex()))
                continue
            while len(data) >= 7:
                l, ms, as_, mc, sc, t = struct.unpack("<HBBBBB", data[:7])
                body = data[7:l]
                cmd = mc << 8 | sc
                out("<- %04x %s t%d m%d a%d len%d: %s  %s" % (cmd, NAMES.get(cmd, "?"), t, ms, as_, l,
                    body[:64].hex(), decode(cmd, t, body)))
                data = data[l:] if l >= 7 else b""

def bcd(number):
    digits = number.lstrip("+")
    if len(digits) % 2:
        digits += "F"
    return bytes(int(digits[i + 1] + digits[i], 16) for i in range(0, len(digits), 2))

def gsm7(text):
    """Pack text of the GSM default alphabet's ASCII subset into septets."""
    bits, n, out = 0, 0, bytearray()
    for ch in text:
        bits |= (ord(ch) & 0x7f) << n
        n += 7
        while n >= 8:
            out.append(bits & 0xff)
            bits >>= 8
            n -= 8
    if n:
        out.append(bits & 0xff)
    return bytes(out)

def sms_submit(number, text):
    """An SMS-SUBMIT TPDU: international number, default alphabet, no validity period."""
    digits = number.lstrip("+")
    return (bytes([0x01, 0x00, len(digits), 0x91]) + bcd(digits) +
            bytes([0x00, 0x00, len(text)]) + gsm7(text))

def send_sms(number, text, smsc=""):
    pdu = sms_submit(number, text)
    sc = (bytes([1 + (len(smsc.lstrip("+")) + 1) // 2, 0x91]) + bcd(smsc)) if smsc else b""
    body = bytes([len(sc)]) + sc + pdu
    # type 2 (outgoing), single message, then the length of what follows
    send(0x0401, 1, bytes([0x02, 0x02, len(body)]) + body)

pump(2)
for step in [a for a in sys.argv[1:] if not a.startswith("/")]:
    if step.startswith("sms="):
        parts = step[4:].split("|")
        send_sms(parts[0], parts[1], parts[2] if len(parts) > 2 else "")
        pump(15)
        continue
    if step.startswith("wait"):
        pump(float(step[4:])); continue
    # CMD:TYPE[:HEXDATA]
    parts = step.split(":")
    send(int(parts[0], 16), int(parts[1]), bytes.fromhex(parts[2]) if len(parts) > 2 else b"")
    pump(1.5)
pump(2)
