# Read the board revision straps as stock's modem driver did: eight pins,
# the first listed the lowest bit (gpg3-3,2,1,0 then gpg1-5,4,3,2).
import fcntl, os, struct
def read(chip, lines):
    fd = os.open("/dev/" + chip, os.O_RDONLY)
    name = fcntl.ioctl(fd, 0x8044B401, bytes(68))[32:64].split(b"\0")[0].decode()
    req = bytearray(struct.pack("64I", *(lines + [0] * (64 - len(lines)))) + b"hwrev".ljust(32, b"\0")
        + struct.pack("QI5I", 4, 0, 0, 0, 0, 0, 0) + bytes(240) + struct.pack("II5Ii", len(lines), 0, 0, 0, 0, 0, 0, 0))
    assert len(req) == 592, len(req)
    fcntl.ioctl(fd, 0xC250B407, req)
    hfd = struct.unpack("i", req[-4:])[0]
    vals = bytearray(struct.pack("QQ", 0, (1 << len(lines)) - 1))
    fcntl.ioctl(hfd, 0xC010B40E, vals)
    bits = struct.unpack("QQ", vals)[0]
    os.close(hfd); os.close(fd)
    return name, [(bits >> i) & 1 for i in range(len(lines))]
n1, a = read("gpiochip13", [3, 2, 1, 0])
n2, b = read("gpiochip5", [5, 4, 3, 2])
bits = a + b
print(n1, a, n2, b, "revision as stock computes it:", sum(v << i for i, v in enumerate(bits)))
