import argparse
import hashlib
import struct
import sys
import zlib
from pathlib import Path

EXE_SHA256 = '216205a09fb00008b37f115820e82c588d083873a34164f41c4d353761a3a844'

DATA_GROUPS = ['Program Executable Files', 'Act One', 'Act Two', 'Act Three', 'Act Four', 'Act Five', 'Video', 'System']
USER_GROUPS = ['English OS Independent Files', 'Language Independent OS Independent Files']


def fail(message):
    print(f'error: {message}', file=sys.stderr)
    sys.exit(1)


def u32(data, offset):
    return struct.unpack_from('<I', data, offset)[0]


def cstr(data, offset):
    return data[offset:data.index(b'\0', offset)].decode('latin-1')


class Iso:
    def __init__(self, path):
        self.path = path
        if not path.is_file():
            fail(f'{path} does not exist')
        self.file = open(path, 'rb')
        self.file.seek(16 * 2048)
        pvd = self.file.read(2048)
        if len(pvd) < 2048 or pvd[0] != 1 or pvd[1:6] != b'CD001':
            fail(f'{path} is not an ISO 9660 disc image')
        self.block = struct.unpack_from('<H', pvd, 128)[0]
        self.files = {}
        self.walk(pvd[156:190])

    def walk(self, record):
        self.file.seek(u32(record, 2) * self.block)
        data = self.file.read(u32(record, 10))
        pos = 0
        while pos < len(data):
            size = data[pos]
            if size == 0:
                pos = (pos // self.block + 1) * self.block
                continue
            entry = data[pos:pos + size]
            pos += size
            name = entry[33:33 + entry[32]]
            if name in (b'\0', b'\1'):
                continue
            if entry[25] & 2:
                self.walk(entry)
            else:
                key = name.decode('latin-1').split(';')[0].rstrip('.').upper()
                self.files.setdefault(key, (u32(entry, 2) * self.block, u32(entry, 10)))

    def read(self, name, offset, size):
        start, length = self.files[name]
        if offset + size > length:
            fail(f'{name} in {self.path} is truncated')
        self.file.seek(start + offset)
        return self.file.read(size)


class Discs:
    def __init__(self, paths):
        self.isos = [Iso(p) for p in paths]

    def find(self, name):
        for iso in self.isos:
            if name in iso.files:
                return iso
        return None

    def read(self, name, offset=0, size=None):
        iso = self.find(name)
        if iso is None:
            fail(f'{name} is not on the given disc images. Pass the images of both Nocturne discs.')
        if size is None:
            size = iso.files[name][1] - offset
        return iso.read(name, offset, size)


class Cabinet:
    def __init__(self, discs, stem):
        self.discs = discs
        self.stem = stem
        hdr = discs.read(f'{stem}1.HDR')
        if u32(hdr, 0) != 0x28635349 or (u32(hdr, 4) >> 12) & 0xf != 5:
            fail(f'{stem}1.HDR is not an InstallShield 5 cabinet header')
        base = u32(hdr, 12)
        table = base + u32(hdr, base + 0x0c)
        dir_count = u32(hdr, base + 0x1c)
        file_count = u32(hdr, base + 0x28)
        offsets = struct.unpack_from(f'<{dir_count + file_count}I', hdr, table)
        self.files = []
        for i in range(file_count):
            p = table + offsets[dir_count + i]
            name_offset, _, flags, expanded, compressed = struct.unpack_from('<IIHII', hdr, p)
            data_offset = u32(hdr, p + 0x26)
            md5 = hdr[p + 0x2a:p + 0x3a]
            self.files.append((cstr(hdr, table + name_offset), flags, expanded, compressed, data_offset, md5))
        self.groups = {}
        for slot in struct.unpack_from('<71I', hdr, base + 0x3e):
            while slot:
                _, descriptor, slot = struct.unpack_from('<3I', hdr, base + slot)
                group = base + descriptor
                first, last = struct.unpack_from('<II', hdr, group + 0x4c)
                self.groups[cstr(hdr, base + u32(hdr, group))] = range(first, last + 1)
        self.volumes = []
        while discs.find(f'{stem}{len(self.volumes) + 1}.CAB'):
            self.volumes.append(struct.unpack_from('<10I', discs.read(f'{stem}{len(self.volumes) + 1}.CAB', 0x14, 40)))
        if not self.volumes or self.volumes[-1][3] < file_count - 1:
            fail(f'{stem}{len(self.volumes) + 1}.CAB is not on the given disc images. Pass the images of both Nocturne discs.')

    def group(self, name):
        if name not in self.groups:
            fail(f'{self.stem}1.HDR has no file group "{name}"')
        return self.groups[name]

    def extract(self, index):
        name, flags, expanded, compressed, data_offset, md5 = self.files[index]
        volume = next((v for v, h in enumerate(self.volumes) if h[2] <= index <= h[3]), None)
        if volume is None:
            fail(f'{name} is on a missing cabinet volume. Pass the images of both Nocturne discs.')
        parts = []
        remaining = compressed
        offset = data_offset
        while remaining > 0:
            if volume >= len(self.volumes):
                fail(f'{name} continues on {self.stem}{volume + 1}.CAB, which is not on the given disc images. Pass the images of both Nocturne discs.')
            header = self.volumes[volume]
            size = remaining
            if index == header[3] and header[9] and header[9] < remaining:
                offset, size = header[7], header[9]
            parts.append(self.discs.read(f'{self.stem}{volume + 1}.CAB', offset, size))
            remaining -= size
            volume += 1
            if remaining > 0:
                offset = self.volumes[volume][4] if volume < len(self.volumes) else 0
        data = b''.join(parts)
        if flags & 4:
            out = []
            pos = 0
            while pos < len(data):
                size = struct.unpack_from('<H', data, pos)[0]
                out.append(zlib.decompressobj(-15).decompress(data[pos + 2:pos + 2 + size]))
                pos += 2 + size
            data = b''.join(out)
        if len(data) != expanded or (any(md5) and hashlib.md5(data).digest() != md5):
            fail(f'{name} failed its checksum. The disc image may be damaged.')
        return name, data


def patch_settings(text):
    lines = text.split('\r\n')
    lines = ['useDirect3D=1' if line.startswith('useDirect3D=') else line for line in lines]
    lines = [line for line in lines if not line.startswith('MaxSwLatency=')]
    lines.insert(lines.index('[Sound]') + 1, 'MaxSwLatency=0.1')
    return '\r\n'.join(lines)


def main():
    parser = argparse.ArgumentParser(description='Extract the Nocturne game files from the disc images into a game folder for nocturne-recomp.')
    parser.add_argument('images', nargs='+', type=Path, help='ISO images of Nocturne disc 1 and disc 2')
    parser.add_argument('--out', type=Path, default=Path(__file__).resolve().parent.parent / 'data' / 'game', help='game folder to create (default: data/game)')
    args = parser.parse_args()
    discs = Discs(args.images)
    out = args.out
    jobs = []
    data = Cabinet(discs, 'DATA')
    for group in DATA_GROUPS:
        for i in data.group(group):
            name = data.files[i][0].lower()
            if name.endswith('.pod') or name == 'nocturne.exe':
                jobs.append((data, i, out))
            elif name == 'opening.avi':
                jobs.append((data, i, out / 'video'))
            elif name == 'render.ini':
                jobs.append((data, i, out / 'system'))
    user = Cabinet(discs, '_USER')
    for group in USER_GROUPS:
        for i in user.group(group):
            name = user.files[i][0].lower()
            if name == 'nocturne.ini':
                jobs.append((user, i, out / 'system'))
            elif group != USER_GROUPS[0] or name.endswith('.pod'):
                jobs.append((user, i, out))
    (out / 'system').mkdir(parents=True, exist_ok=True)
    (out / 'video').mkdir(exist_ok=True)
    for n, (cabinet, index, folder) in enumerate(jobs, 1):
        name = cabinet.files[index][0]
        target = folder / name
        if folder.name == 'system' and any(p.name.lower() == name.lower() for p in folder.iterdir()):
            print(f'[{n}/{len(jobs)}] keeping existing {target}')
            continue
        print(f'[{n}/{len(jobs)}] {target}')
        name, content = cabinet.extract(index)
        if name.lower() == 'nocturne.ini':
            content = patch_settings(content.decode('latin-1')).encode('latin-1')
        if name.lower() == 'nocturne.exe' and hashlib.sha256(content).hexdigest() != EXE_SHA256:
            fail(f'nocturne.exe has SHA-256 {hashlib.sha256(content).hexdigest()}, but the recompiler supports only {EXE_SHA256}')
        target.write_bytes(content)
    print(f'game folder: {out}')


if __name__ == '__main__':
    main()
