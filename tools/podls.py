import struct
import sys


def read_pod(path):
    with open(path, 'rb') as f:
        d = f.read()
    if d[:4] != b'POD2':
        raise ValueError(f'{path}: not a POD2 archive')
    count = struct.unpack_from('<I', d, 0x58)[0]
    table = 0x60
    names = table + count * 20
    files = []
    for i in range(count):
        name_off, size, offset, _, _ = struct.unpack_from('<IIIII', d, table + 20 * i)
        end = d.index(b'\0', names + name_off)
        files.append((d[names + name_off:end].decode('latin-1'), size, offset))
    return d, files


if __name__ == '__main__':
    _, files = read_pod(sys.argv[1])
    for name, size, offset in files:
        print(f'{size:9d} {name}')
