import sys

lo, hi = 0x401000, 0x401000 + 0x172c00
cov = bytearray(hi - lo)
nf = 0
for line in open(sys.argv[1]):
    p = line.split()
    if p[0] != 'func':
        continue
    nf += 1
    for r in p[3:]:
        if '-' not in r:
            continue
        a, b = (int(x, 16) for x in r.split('-'))
        cov[max(a, lo) - lo:min(b, hi) - lo] = b'\x01' * max(0, min(b, hi) - max(a, lo))
total = hi - lo
c = sum(cov)
print(f'functions {nf}, covered {c} of {total} bytes ({100 * c / total:.1f}%)')
gaps = []
start = None
for i, v in enumerate(cov + b'\x01'):
    if not v and start is None:
        start = i
    elif v and start is not None:
        gaps.append((start + lo, i - start))
        start = None
big = sorted((g for g in gaps if g[1] > 64), key=lambda g: -g[1])
print('gaps over 64 bytes:', len(big), 'bytes in them:', sum(g[1] for g in big))
for a, n in big[:int(sys.argv[2]) if len(sys.argv) > 2 else 10]:
    print(f'  {a:08x} {n}')
