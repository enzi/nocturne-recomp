import struct
import sys

src, dst = sys.argv[1], sys.argv[2]
d = bytearray(open(src, 'rb').read())
pe = struct.unpack_from('<I', d, 0x3c)[0]
nsec = struct.unpack_from('<H', d, pe + 6)[0]
optsz = struct.unpack_from('<H', d, pe + 20)[0]
sh = pe + 24 + optsz
for i in range(nsec):
    off = sh + 40 * i
    vsize, vaddr, rawsize, rawptr = struct.unpack_from('<IIII', d, off + 8)
    if vsize == 0:
        struct.pack_into('<I', d, off + 8, rawsize)
    if rawptr == 0:
        struct.pack_into('<I', d, off + 16, 0)
open(dst, 'wb').write(d)
