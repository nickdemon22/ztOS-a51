# Try AT commands on a modem channel; print whatever comes back.
import os, select, sys, time
dev = sys.argv[1]
fd = os.open(dev, os.O_RDWR | os.O_NONBLOCK)
keep = [os.open("/dev/umts_ipc0", os.O_RDWR | os.O_NONBLOCK), os.open("/dev/umts_rfs0", os.O_RDWR | os.O_NONBLOCK)]
def pump(secs):
    end = time.time() + secs
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.2)
        if r:
            data = os.read(fd, 4096)
            print("<- %r" % data, flush=True)
for cmd in sys.argv[2:]:
    os.write(fd, (cmd + "\r").encode())
    print("-> %s" % cmd, flush=True)
    pump(3)
