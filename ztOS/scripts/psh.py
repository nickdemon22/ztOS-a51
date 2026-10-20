#!/usr/bin/env python3
"""Run one shell command on the phone's zboot rescue shell (telnet, USB network).

  scripts/psh.py [-t seconds] '<command>'

Prints the command's output and exits with its status.
"""
import socket
import sys
import time

HOST, PORT = "172.16.42.1", 23
timeout = 30.0
argv = sys.argv[1:]
if argv[:1] == ["-t"]:
    timeout = float(argv[1])
    argv = argv[2:]
cmd = " ".join(argv)
MARK = "__ZB_DONE__"

s = socket.create_connection((HOST, PORT), timeout=10)
s.settimeout(1.0)
buf = b""


def strip_iac(data):
    """Refuse every telnet option; return the plain bytes."""
    out = bytearray()
    i = 0
    while i < len(data):
        if data[i] == 255 and i + 2 < len(data) + 1:
            if i + 1 < len(data) and data[i + 1] in (251, 252, 253, 254) and i + 2 < len(data):
                verb, opt = data[i + 1], data[i + 2]
                s.sendall(bytes([255, 254 if verb in (251, 252) else 252, opt]))
                i += 3
                continue
            i += 2
            continue
        out.append(data[i])
        i += 1
    return bytes(out)


def pump(until, limit):
    global buf
    end = time.time() + limit
    while time.time() < end:
        try:
            chunk = s.recv(65536)
        except socket.timeout:
            continue
        if not chunk:
            break
        buf += strip_iac(chunk)
        if until in buf:
            return True
    return False


time.sleep(0.5)
pump(b"\x00never", 1.0)
buf = b""
# the marker is split in the command so its echo never matches
s.sendall(f"stty -echo 2>/dev/null; PS1=; {cmd}; echo __ZB_''DONE__$?\n".encode())
ok = pump(MARK.encode(), timeout)
text = buf.decode("utf-8", "replace").replace("\r\n", "\n")
# drop the echoed command line if the tty echoed it before stty took effect
if "DONE__$?" in text:
    text = text.split("DONE__$?", 1)[1].lstrip("\n")
rc = 124
if ok:
    head, _, tail = text.rpartition(MARK)
    text = head
    digits = "".join(ch for ch in tail[:4] if ch.isdigit())
    rc = int(digits) if digits else 0
sys.stdout.write(text)
s.close()
sys.exit(rc)
