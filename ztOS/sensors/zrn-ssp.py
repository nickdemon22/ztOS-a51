#!/usr/bin/env python3
"""zrn-ssp: talk Samsung's SSP protocol to the sensor hub through /dev/zrn_ssp.

  zrn-ssp.py info                    chip id, firmware revision, sensors found
  zrn-ssp.py watch SENSOR[:MS[:BATCH_MS]] ...   switch sensors on and print what they send
  zrn-ssp.py raw                     print every frame the hub sends

The phone starts a transaction with a 9-byte header (command, length, options,
data); the hub answers with the options echoed and then either takes the
payload (write) or sends the reply (read). Everything the hub sends on its own
initiative arrives as a "hub to phone" frame made of tagged records.
"""
import os
import select
import struct
import sys
import time

DEV = "/dev/zrn_ssp"
READ, WRITE, HUB2AP = 0, 1, 2

SENSORS = {
    "accel": 0, "gyro": 1, "mag_uncal": 2, "mag_raw": 3, "mag": 4, "pressure": 5,
    "gesture": 6, "prox": 7, "light": 9, "prox_raw": 10, "orientation": 12,
    "step_det": 13, "sig_motion": 14, "gyro_uncal": 15, "game_rot": 16, "rot": 17,
    "step_cnt": 18, "light_ir": 24, "int_gyro": 25, "tilt": 26, "pickup": 27,
    "light_flicker": 31, "light_cct": 32, "accel_uncal": 33,
}
NAMES = {v: k for k, v in SENSORS.items()}


class Hub:
    def __init__(self):
        self.fd = os.open(DEV, os.O_RDWR)
        self.buf = b""
        self.index = 1
        self.pending = {}   # options -> payload (write) or length (read)
        self.replies = {}
        self.frames = []

    def send(self, cmd, payload=b"", read=0):
        kind = READ if read else WRITE
        opts = kind | (self.index << 3)
        self.index = self.index % 8000 + 1
        length = read if read else len(payload)
        self.pending[opts] = read if read else payload
        os.write(self.fd, struct.pack("<BHHI", cmd, length, opts, 0))
        return opts

    def pump(self, timeout):
        """Read what is there for up to `timeout` seconds, handle transactions."""
        end = time.time() + timeout
        while True:
            left = end - time.time()
            if left <= 0 or not select.select([self.fd], [], [], left)[0]:
                return
            chunk = os.read(self.fd, 8192)
            # the device hands out whole packets, each behind a 2-byte length
            while len(chunk) >= 2:
                n = struct.unpack_from("<H", chunk)[0]
                self.buf += chunk[2:2 + n]
                chunk = chunk[2 + n:]
            self.parse()

    def parse(self):
        while len(self.buf) >= 4:
            opts, length = struct.unpack_from("<HH", self.buf)
            kind = opts & 3
            if kind == HUB2AP:
                if len(self.buf) < 4 + length:
                    return
                self.frames.append(self.buf[4:4 + length])
                self.buf = self.buf[4 + length:]
            elif kind == WRITE and opts in self.pending:
                os.write(self.fd, self.pending.pop(opts))
                self.replies[opts] = b""
                self.buf = self.buf[4:]
            elif kind == READ and opts in self.pending:
                want = self.pending[opts]
                if len(self.buf) < 4 + want:
                    return
                del self.pending[opts]
                self.replies[opts] = self.buf[4:4 + want]
                self.buf = self.buf[4 + want:]
            else:
                print(f"? unexpected header {self.buf[:4].hex()}, dropping {len(self.buf)} bytes")
                self.buf = b""

    def call(self, cmd, payload=b"", read=0, timeout=1.0):
        opts = self.send(cmd, payload, read)
        end = time.time() + timeout
        while opts not in self.replies and time.time() < end:
            self.pump(0.05)
        return self.replies.pop(opts, None)

    def add(self, sensor, ms, batch_ms=0):
        return self.call(0xA1, struct.pack("<BiiB", sensor, ms, batch_ms, 0))

    def remove(self, sensor):
        return self.call(0xA2, struct.pack("<Bi", sensor, 0))


def records(frame, sizes):
    """Split a hub frame into (tag, sensor, data) records as far as it is understood."""
    i = 0
    while i < len(frame):
        tag = frame[i]; i += 1
        if tag == 0x37:
            sensor = frame[i]; i += 1
            size = sizes.get(sensor)
            if size is None:
                yield ("data?", sensor, frame[i:])
                return
            yield ("data", sensor, frame[i:i + size])
            i += size + 8
        elif tag == 0x03:
            n = struct.unpack_from("<H", frame, i)[0]; i += 2
            yield ("debug", None, frame[i:i + n])
            i += n
        elif tag == 0x05:
            yield ("meta", frame[i + 1], frame[i:i + 2]); i += 2
        elif tag == 0x0A:
            yield ("init-done", None, b"")
        elif tag == 0x06:
            yield ("time-sync", None, b"")
        else:
            yield (f"tag {tag:#04x}", None, frame[i:])
            return


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else "info"
    hub = Hub()
    hub.pump(0.3)
    if mode == "info":
        for name, cmd, n in (("chip id", 0x0F, 1), ("firmware", 0xF0, 4), ("sensors", 0xF4, 8)):
            r = hub.call(cmd, read=n)
            print(f"{name:9s} {r.hex() if r is not None else 'no answer'}"
                  + (f" = {int.from_bytes(r, 'little')}" if r else ""))
            if name == "sensors" and r:
                state = int.from_bytes(r, "little")
                print("          " + " ".join(NAMES.get(b, str(b)) for b in range(64) if state >> b & 1))
        return
    sizes = {}
    wanted = []
    for arg in sys.argv[2:]:
        name, _, ms = arg.partition(":")
        ms, _, batch = ms.partition(":")
        sensor = SENSORS.get(name, None)
        sensor = int(name) if sensor is None else sensor
        wanted.append(sensor)
        hub.call(0xD1)  # the phone is awake
        print(f"add {NAMES.get(sensor, sensor)}: {hub.add(sensor, int(ms or 200), int(batch or 0))!r}")
    try:
        while True:
            hub.pump(0.5)
            for frame in hub.frames:
                if mode == "raw" or True:
                    for kind, sensor, data in records(frame, sizes):
                        if kind == "debug":
                            print("  hub:", data.decode("ascii", "replace").strip())
                        else:
                            print(f"  {kind} {NAMES.get(sensor, sensor)} {data.hex()}")
            hub.frames.clear()
    except KeyboardInterrupt:
        pass
    finally:
        for sensor in wanted:
            hub.remove(sensor)


main()
