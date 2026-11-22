import math
import struct

from geo import parse_geo


class Pod:
    def __init__(self, path):
        with open(path, 'rb') as f:
            self.data = f.read()
        count = struct.unpack_from('<I', self.data, 0x58)[0]
        names = 0x60 + count * 20
        self.files = {}
        for i in range(count):
            no, size, off, _, _ = struct.unpack_from('<IIIII', self.data, 0x60 + 20 * i)
            end = self.data.index(b'\0', names + no)
            self.files[self.data[names + no:end].decode('latin-1').upper()] = (size, off)

    def read(self, name):
        size, off = self.files[name.upper()]
        return self.data[off:off + size]


def parse_cameras(set_text):
    lines = [l.strip() for l in set_text.splitlines()]
    cams = []
    for i, l in enumerate(lines):
        if l.startswith('-- camera name'):
            name = lines[i + 1]
            pos = tuple(map(float, lines[lines.index('pos', i) + 1].split(',')))
            fov = float(lines[lines.index('fov', i) + 1])
            k = lines.index('vmat', i)
            rows = [tuple(map(float, lines[k + 1 + r].split(','))) for r in range(3)]
            cams.append({'name': name, 'pos': pos, 'fov': fov, 'm': rows})
    return cams


def to_camera(cam, p):
    d = [p[i] - cam['pos'][i] for i in range(3)]
    m = cam['m']
    return tuple(sum(m[r][col] * d[r] for r in range(3)) for col in range(3))
