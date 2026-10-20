# SPDX-License-Identifier: GPL-2.0-or-later
"""ztos-zygote: Qt for Python loaded once, and every SDK app started from it.

Loading Qt for Python costs a second of CPU on the phone's small cores, every
time an app starts. This process loads it once and then sleeps on a socket;
an app's launcher hands it the app (its file, arguments, environment and its
stdin/stdout/stderr) and it forks: the child already has Qt, and only runs the
app. Asleep it costs nothing; its memory is shared with the apps forked from
it until they write to it.

Nothing GUI is made here: a Wayland connection, a QApplication or a thread
cannot survive a fork, so each app makes its own. What is loaded is only code.

Protocol, on $XDG_RUNTIME_DIR/ztos-zygote (a stream socket, this user only):
the launcher sends "ztos1", a 4-byte length, then BINARY CWD ARGC ARG... ENV...
(NAME=VALUE), each ending in a zero byte; its stdin, stdout and stderr go
with it (SCM_RIGHTS)
and gets back "pid N\\n" when the app is forked, then "exit N\\n" when it ends.
"""
import array
import os
import signal
import socket
import sys

# what every app needs, loaded once (the reason this exists)
import PySide6.QtCore  # noqa: F401
import PySide6.QtGui  # noqa: F401
import PySide6.QtWidgets  # noqa: F401
import PySide6.QtOpenGL  # noqa: F401
import PySide6.QtSvg  # noqa: F401
import json, math, struct, time, traceback, zipfile, runpy  # noqa: E401,F401 (theirs too)
import hashlib

# the fonts: fontconfig reads its configuration and cache once, here (Qt finds
# it ready in every app; it needs no window to load)
try:
    import ctypes
    ctypes.CDLL("libfontconfig.so.1").FcInit()
except OSError:
    pass

# the SDK's runtime as installed with the apps: an app whose own copy is the
# same file uses it as it is (see the app's __main__), else loads its own
RUNTIME_DIR = os.path.expanduser("~/.local/share/ztos/lib")
try:
    sys.path.insert(0, RUNTIME_DIR)
    import ztos  # noqa: F401
    with open(os.path.join(RUNTIME_DIR, "ztos", "__init__.py"), "rb") as f:
        os.environ["ZTOS_PRELOADED"] = hashlib.sha256(f.read()).hexdigest()
except Exception:
    traceback.print_exc()
    for name in [m for m in sys.modules if m == "ztos" or m.startswith("ztos.")]:
        del sys.modules[name]
finally:
    if sys.path and sys.path[0] == RUNTIME_DIR:
        sys.path.pop(0)
PRELOADED = os.environ.pop("ZTOS_PRELOADED", "")

VERSION = b"ztos1"
children = {}   # pid -> the launcher's connection


def path() -> str:
    runtime = os.environ.get("XDG_RUNTIME_DIR") or f"/run/user/{os.getuid()}"
    return os.path.join(runtime, "ztos-zygote")


def receive(conn):
    """One request: (fields, descriptors). It starts "ztos1", then its length
    (4 bytes, little-endian), then the fields, each ending in a zero byte."""
    fds = array.array("i")
    data = b""
    want = None
    while want is None or len(data) < want:
        msg, anc, flags, _ = conn.recvmsg(65536, socket.CMSG_SPACE(3 * fds.itemsize))
        for level, kind, payload in anc:
            if level == socket.SOL_SOCKET and kind == socket.SCM_RIGHTS:
                fds.frombytes(payload[:len(payload) - len(payload) % fds.itemsize])
        if not msg:
            raise OSError("the launcher went away")
        data += msg
        if want is None and len(data) >= 9:
            if data[:5] != VERSION:
                raise OSError("not a ztos launcher")
            want = 9 + struct.unpack("<I", data[5:9])[0]
    return data[9:want].split(b"\0")[:-1], list(fds)


def run_child(fields, fds):
    """In the forked child: become the app. Never returns."""
    try:
        binary, cwd, argc = fields[0].decode(), fields[1].decode(), int(fields[2])
        argv = [f.decode() for f in fields[3:3 + argc]]
        env = dict(f.decode().split("=", 1) for f in fields[3 + argc:] if b"=" in f)
        for i, fd in enumerate(fds[:3]):
            os.dup2(fd, i)
        for fd in fds:
            if fd > 2:
                os.close(fd)
        signal.signal(signal.SIGCHLD, signal.SIG_DFL)
        signal.signal(signal.SIGTERM, signal.SIG_DFL)
        os.environ.clear()
        os.environ.update(env)
        os.environ["ZTOS_PRELOADED"] = PRELOADED     # which runtime is already loaded
        os.chdir(cwd)
        sys.argv = [binary] + argv
        sys.stdin = open(0, closefd=False)
        sys.stdout = open(1, "w", buffering=1, closefd=False)
        sys.stderr = open(2, "w", buffering=1, closefd=False)
        runpy.run_path(binary, run_name="__main__")
        code = 0
    except SystemExit as e:
        code = e.code if isinstance(e.code, int) else (0 if e.code is None else 1)
    except BaseException:
        traceback.print_exc()
        code = 1
    try:
        sys.stdout.flush()
        sys.stderr.flush()
    finally:
        os._exit(code)


def reap(*_):
    while True:
        try:
            pid, status = os.waitpid(-1, os.WNOHANG)
        except ChildProcessError:
            return
        if pid == 0:
            return
        conn = children.pop(pid, None)
        if conn is not None:
            try:
                conn.sendall(f"exit {os.waitstatus_to_exitcode(status)}\n".encode())
            except OSError:
                pass
            conn.close()


def main():
    where = path()
    server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        os.unlink(where)
    except FileNotFoundError:
        pass
    old = os.umask(0o077)
    server.bind(where)
    os.umask(old)
    server.listen(8)
    signal.signal(signal.SIGCHLD, reap)
    while True:
        try:
            conn, _ = server.accept()
        except InterruptedError:
            continue
        try:
            # only this user (the socket's folder is already only theirs)
            creds = conn.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, struct.calcsize("3i"))
            if struct.unpack("3i", creds)[1] != os.getuid():
                conn.close()
                continue
            fields, fds = receive(conn)
        except OSError:
            conn.close()
            continue
        signal.pthread_sigmask(signal.SIG_BLOCK, {signal.SIGCHLD})
        pid = os.fork()
        if pid == 0:
            server.close()
            conn.close()
            signal.pthread_sigmask(signal.SIG_UNBLOCK, {signal.SIGCHLD})
            run_child(fields, fds)
        children[pid] = conn
        signal.pthread_sigmask(signal.SIG_UNBLOCK, {signal.SIGCHLD})
        for fd in fds:
            os.close(fd)
        try:
            conn.sendall(f"pid {pid}\n".encode())
        except OSError:
            pass
        reap()


if __name__ == "__main__":
    main()
