import struct


def parse_geo(g):
    version, nx, ny, nz = struct.unpack_from('<4i', g, 0)
    bounds_min = struct.unpack_from('<3f', g, 0x10)
    bounds_max = struct.unpack_from('<3f', g, 0x1c)
    cell_size = struct.unpack_from('<3f', g, 0x28)
    o = 0x34
    cells = []
    for _ in range(nx * ny * nz):
        cmin = struct.unpack_from('<3f', g, o)
        cmax = struct.unpack_from('<3f', g, o + 12)
        nv, nf = struct.unpack_from('<2i', g, o + 24)
        o += 32
        verts = [struct.unpack_from('<3f', g, o + 12 * i) for i in range(nv)]
        o += 12 * nv
        faces = []
        for i in range(nf):
            a, b, c = struct.unpack_from('<3i', g, o)
            plane = struct.unpack_from('<4f', g, o + 12)
            extra = struct.unpack_from('<i', g, o + 28)[0]
            faces.append((a, b, c, plane, extra))
            o += 32
        surface = list(g[o:o + nf]) if nf else []
        o += nf
        if nf:
            o += 64
        cells.append({'min': cmin, 'max': cmax, 'verts': verts, 'faces': faces, 'surface': surface})
    return {'version': version, 'grid': (nx, ny, nz), 'min': bounds_min, 'max': bounds_max,
            'cell': cell_size, 'cells': cells, 'end': o}
