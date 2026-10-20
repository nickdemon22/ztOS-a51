#!/usr/bin/env python3
"""Run one command over ssh with a password, without sshpass or pexpect.

  scripts/ssh-pw.py <user> <password[,password2,...]> <host> '<command>'

ssh insists on a terminal for the password prompt, so it is given one. More
than one password is answered in order, which is what `su -c` inside the
command needs (the login password, then root's).
"""
import os
import pty
import sys
import select

user, password, host, cmd = sys.argv[1], sys.argv[2], sys.argv[3], " ".join(sys.argv[4:])
argv = [
    "ssh", "-o", "StrictHostKeyChecking=no", "-o", "UserKnownHostsFile=/dev/null",
    "-o", "PreferredAuthentications=password,keyboard-interactive",
    "-o", "PubkeyAuthentication=no", "-o", "ConnectTimeout=10",
    f"{user}@{host}", cmd,
]
pid, fd = pty.fork()
if pid == 0:
    os.execvp("ssh", argv)

passwords = password.split(",")
out = b""
seen = 0
cursor = 0
while True:
    r, _, _ = select.select([fd], [], [], 120)
    if not r:
        break
    try:
        chunk = os.read(fd, 65536)
    except OSError:
        break
    if not chunk:
        break
    out += chunk
    while seen < len(passwords):
        hit = out.find(b"assword", cursor)
        if hit < 0:
            break
        cursor = hit + 7
        os.write(fd, passwords[seen].encode() + b"\n")
        seen += 1
_, status = os.waitpid(pid, 0)
text = out.decode("utf-8", "replace").replace("\r\n", "\n")
# drop everything up to and including the password prompt line
if seen and "assword" in text:
    text = text.split("assword", 1)[1].split("\n", 1)[-1]
sys.stdout.write(text)
sys.exit(os.waitstatus_to_exitcode(status))
