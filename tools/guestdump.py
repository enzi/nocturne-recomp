import bisect
import struct


class GuestDump:
    def __init__(self, path):
        with open(path, 'rb') as f:
            data = f.read()
        self.blocks = []
        o = 0
        while o < len(data):
            addr, size = struct.unpack_from('<II', data, o)
            self.blocks.append((addr, data[o + 8:o + 8 + size]))
            o += 8 + size
        self.blocks.sort(key=lambda b: b[0])
        self.starts = [b[0] for b in self.blocks]

    def read(self, addr, n):
        i = bisect.bisect_right(self.starts, addr) - 1
        if i < 0:
            raise KeyError(hex(addr))
        base, blob = self.blocks[i]
        if addr + n > base + len(blob):
            raise KeyError(hex(addr))
        return blob[addr - base:addr - base + n]

    def u32(self, addr):
        return struct.unpack('<I', self.read(addr, 4))[0]

    def f32(self, addr):
        return struct.unpack('<f', self.read(addr, 4))[0]

    def hero(self):
        return self.u32(0x01cac0c8 + 4 * self.u32(0x01cac0d8))
