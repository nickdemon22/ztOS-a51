#!/usr/bin/env python3
"""Run deep sleep test with automated host monitoring and wake alarm if the phone hangs."""
import subprocess
import time
import sys

def probe():
    try:
        r = subprocess.run(["python3", "/tmp/ztos-phone.py", "probe"], capture_output=True, text=True, timeout=3)
        return "22 open" in r.stdout
    except Exception:
        return False

def play_alarm():
    print("\n" + "!" * 60)
    print("PHONE STOPPED RESPONDING! PLAYING ALARM TO WAKE USER!")
    print("!" * 60 + "\n")
    for _ in range(20):
        try:
            subprocess.run(["paplay", "/usr/share/sounds/freedesktop/stereo/alarm-clock-elapsed.oga"])
        except Exception:
            subprocess.run(["speaker-test", "-t", "sine", "-f", "1000", "-l", "2"])
        time.sleep(1)

def run_cmd(cmd):
    return subprocess.run(["python3", "/tmp/ztos-phone.py", "root", cmd], capture_output=True, text=True, timeout=20)

print("[1] Verifying phone connection...")
if not probe():
    print("Error: phone not reachable on port 22")
    sys.exit(1)
print("Phone is reachable.")

print("[2] Checking firmware state before test...")
r = run_cmd("python3 /usr/local/libexec/ztos-power/firmware-state.py; cat /sys/devices/virtual/sec/sensorhub/ready")
print("Firmware state:", r.stdout.strip())

print("[3] Launching deep sleep test (10s RTC alarm, USB detached)...")
unit = f"ztos-deep-sleep-{int(time.time())}"
launch_cmd = f"systemd-run --unit={unit} --collect /bin/sh /tmp/ztos-usb-sleep-test.sh"
r = run_cmd(launch_cmd)
print("Launched:", r.stdout.strip())

print("[4] Waiting for 10s sleep cycle + 5s settle...")
time.sleep(12)

deadline = time.time() + 25
woke = False
while time.time() < deadline:
    if probe():
        woke = True
        print(f"Phone responded at t+{round(time.time() - (deadline - 25), 1)}s!")
        break
    print("Waiting for phone to reattach USB...", round(deadline - time.time(), 1), "s remaining")
    time.sleep(2)

if not woke:
    print("FAILED: Phone did not wake up within timeout!")
    play_alarm()
    sys.exit(1)

print("[5] Fetching test results and logs...")
time.sleep(2)
r = run_cmd(f"journalctl -u {unit} --no-pager; python3 /usr/local/libexec/ztos-power/firmware-state.py; cat /sys/devices/virtual/sec/sensorhub/ready")
print("=== LOG OUTPUT ===")
print(r.stdout)
print("==================")
