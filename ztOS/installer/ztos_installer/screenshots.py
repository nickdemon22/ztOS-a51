# SPDX-License-Identifier: GPL-2.0-or-later
"""Screenshots of ztOS, taken from the emulator itself: the image is booted,
ztMETRO is driven through its control pipe (the same one ztos-sdk uses), and
each screen is saved with QEMU's own screen dump, so nothing of the host's
graphics or any real phone is involved.

    ztos screenshots [--out docs/screenshots]
"""
from __future__ import annotations

import os
import shutil
import socket
import subprocess
import time
from pathlib import Path

from . import targets as t
from .build import ROOT, WORK, Fail, say

SSH_OPTS = ["-p", "2222", "-o", "StrictHostKeyChecking=no", "-o", "UserKnownHostsFile=/dev/null",
            "-o", "LogLevel=ERROR", "-o", "ConnectTimeout=10", "-o", "PreferredAuthentications=password",
            "-o", "PubkeyAuthentication=no"]


class Vm:
    def __init__(self, user: str, password: str):
        self.user, self.password = user, password
        self.monitor = WORK / "qemu" / "monitor.sock"
        self.proc: subprocess.Popen | None = None

    # -- the machine --
    def start(self):
        self.monitor.unlink(missing_ok=True)
        cmd = t.qemu_command(WORK / "qemu" / "ztos-qemu.img", headless=True, monitor=self.monitor)
        say("booting ztOS (about four minutes: the phone is emulated)")
        self.proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def stop(self):
        if self.proc is not None:
            try:
                self.run("sudo -S -p '' systemctl poweroff", sudo=True, quiet=True)
                self.proc.wait(timeout=60)
            except Exception:
                self.proc.kill()

    # -- ssh --
    def run(self, command: str, sudo: bool = False, quiet: bool = False, timeout: int = 90) -> str:
        env = dict(os.environ, SSH_ASKPASS=str(ROOT / "sdk" / "bin" / "ztos-askpass"), SSH_ASKPASS_REQUIRE="force",
                   ZTOS_PASSWORD=self.password, DISPLAY=os.environ.get("DISPLAY", ":0"))
        stdin = (self.password + "\n").encode() if sudo else None
        r = subprocess.run(["ssh", *SSH_OPTS, f"{self.user}@127.0.0.1", command], input=stdin, env=env,
                           capture_output=True, timeout=timeout)
        if r.returncode != 0 and not quiet:
            raise Fail(f"in the emulator: {r.stderr.decode(errors='replace').strip()[:300]}")
        return r.stdout.decode(errors="replace")

    def wait_ssh(self, minutes: int = 12):
        end = time.monotonic() + minutes * 60
        while time.monotonic() < end:
            if self.proc.poll() is not None:
                raise Fail("the emulator stopped")
            try:
                self.run("true", quiet=False, timeout=30)
                return
            except (Fail, subprocess.TimeoutExpired):
                time.sleep(10)
        raise Fail("the emulator did not answer over ssh")

    # -- the phone's screen --
    def ctl(self, line: str):
        self.run(f"printf '%s\\n' {shell_quote(line)} > /run/user/$(id -u)/ztmetro.ctl")

    def shot(self, path: Path):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.unlink(missing_ok=True)
        with socket.socket(socket.AF_UNIX) as s:
            s.settimeout(10)
            s.connect(str(self.monitor))
            s.recv(4096)
            s.sendall(f"screendump {path} -f png\n".encode())
            time.sleep(2)
        if not path.exists():
            raise Fail(f"the emulator did not save {path}")

    def wait_shell(self, minutes: int = 15):
        end = time.monotonic() + minutes * 60
        while time.monotonic() < end:
            out = self.run("test -p /run/user/$(id -u)/ztmetro.ctl && echo yes", quiet=True)
            if "yes" in out:
                return
            time.sleep(10)
        raise Fail("ztMETRO did not start in the emulator (see build/ztos/qemu/console.log)")


def shell_quote(s: str) -> str:
    return "'" + s.replace("'", "'\\''") + "'"


def drag(vm: Vm, x0, y0, x1, y1, steps=8):
    """A finger from (x0, y0) to (x1, y1), in points, as the shell's own test driver does."""
    lines = [f"down 1 {x0} {y0}"]
    for i in range(1, steps + 1):
        lines.append(f"move 1 {x0 + (x1 - x0) * i // steps} {y0 + (y1 - y0) * i // steps}")
    lines.append("up 1")
    vm.run("for l in " + " ".join(shell_quote(l) for l in lines) +
           "; do printf '%s\\n' \"$l\" > /run/user/$(id -u)/ztmetro.ctl; sleep 0.05; done")


def take(out: Path, user: str = "ztos", password: str = "ztos", keep: bool = False):
    if not (WORK / "qemu" / "ztos-qemu.img").exists():
        raise Fail("no emulator disk: ztos build --target qemu")
    vm = Vm(user, password)
    try:
        vm.start()
        vm.wait_ssh()
        say("booted; waiting for ztMETRO")
        vm.wait_shell()
        time.sleep(20)
        say("the first start: the lock screen")
        vm.ctl("screen on")
        time.sleep(3)
        vm.shot(out / "lock.png")
        drag(vm, 180, 620, 180, 160)
        time.sleep(8)
        vm.shot(out / "first-start-welcome.png")           # the Welcome app opens by itself on the first start
        for n in range(1, 5):                              # its pages: tap next
            vm.ctl("tap 180 700")
            time.sleep(6)
            vm.shot(out / f"welcome-{n}.png")
        vm.ctl("tap 180 700")                              # to the last page: done
        time.sleep(6)
        vm.ctl("tap 180 330")                              # "start using ztOS"
        time.sleep(10)
        vm.shot(out / "start.png")
        drag(vm, 300, 300, 60, 300)
        time.sleep(6)
        vm.shot(out / "app-list.png")
        vm.ctl("tap 60 722")
        time.sleep(4)
        vm.ctl("launch ztsettings")
        time.sleep(25)
        vm.shot(out / "settings.png")
        vm.ctl("osk show")
        time.sleep(4)
        vm.shot(out / "keyboard-over-settings.png")
        vm.ctl("osk hide")
        vm.ctl("tap 60 722")
        time.sleep(3)
        vm.ctl("launch /usr/local/lib/ztos/apps/com.zerone.music")
        time.sleep(4)
        vm.shot(out / "loading.png")
        time.sleep(25)
        vm.shot(out / "music.png")
        vm.ctl("tap 60 722")
        time.sleep(3)
        drag(vm, 180, 8, 180, 520, steps=12)
        time.sleep(5)
        vm.shot(out / "action-centre.png")
    finally:
        if not keep:
            vm.stop()
    say(f"screenshots in {out}")
