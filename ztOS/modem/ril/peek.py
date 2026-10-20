# peek.py ADDR...: read 32-bit registers through /dev/mem
import mmap, os, struct, sys
fd = os.open("/dev/mem", os.O_RDONLY | os.O_SYNC)
for a in sys.argv[1:]:
    addr = int(a, 16)
    m = mmap.mmap(fd, 4096, mmap.MAP_SHARED, mmap.PROT_READ, offset=addr & ~0xfff)
    print("%08x = %08x" % (addr, struct.unpack("<I", m[addr & 0xfff:(addr & 0xfff) + 4])[0]))
    m.close()
