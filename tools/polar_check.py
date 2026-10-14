"""Independent stereo check for a recorded WAV: no Holotoner code.

Usage: python3 polar_check.py in.wav [out.wav ...] -o plot.png
Each file gets two goniometer-style half-disc plots of every sample:
  angle  = atan2(R - L, L + R), folded so a sample and its negative coincide
  radius = left: linear |(L, R)| / peak;  right: dB from -48 to 0 relative to peak
Prints correlation and side/mid energy so changes are measurable, not just visible.
"""
import math, struct, sys, wave, zlib

def read(path):
    with wave.open(path, 'rb') as w:
        ch, width, n = w.getnchannels(), w.getsampwidth(), w.getnframes()
        data = w.readframes(n)
    assert ch == 2 and width == 2, 'expects 16-bit stereo'
    v = struct.unpack('<%dh' % (2 * n), data)
    return [x / 32768 for x in v[0::2]], [x / 32768 for x in v[1::2]]

def stats(L, R):
    ll = sum(a * a for a in L); rr = sum(b * b for b in R); lr = sum(a * b for a, b in zip(L, R))
    mid = sum((a + b) ** 2 for a, b in zip(L, R)); side = sum((b - a) ** 2 for a, b in zip(L, R))
    return lr / math.sqrt(ll * rr + 1e-30), 10 * math.log10(side / (mid + 1e-30) + 1e-30), 10 * math.log10(ll / (rr + 1e-30))

def plot(L, R, size, log):
    img = [[0] * (2 * size) for _ in range(size)]
    peak = max(max(abs(a), abs(b)) for a, b in zip(L, R)) * math.sqrt(2) + 1e-12
    for a, b in zip(L, R):
        s, d = a + b, b - a
        if s < 0: s, d = -s, -d
        level = math.hypot(a, b) / peak
        if level < 1e-6: continue
        r = level if not log else max(0.0, 1 + 20 * math.log10(level) / 48)
        ang = math.atan2(d, s)
        x = int(size + r * math.sin(ang) * (size - 1)); y = int(size - 1 - r * math.cos(ang) * (size - 1))
        if 0 <= x < 2 * size and 0 <= y < size: img[y][x] = min(255, img[y][x] + 40)
    return img

def png(rows, path):
    h, w = len(rows), len(rows[0])
    raw = b''.join(b'\x00' + bytes(r) for r in rows)
    chunk = lambda t, d: struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 0, 0, 0, 0))
                           + chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))

args = sys.argv[1:]
out = args[args.index('-o') + 1] if '-o' in args else 'polar_check.png'
files = [a for a in args if a.endswith('.wav') and a != out]
size, rows = 180, []
for path in files:
    L, R = read(path)
    c, sm, lr = stats(L, R)
    print(f'{path}: correlation {c:+.3f}, side/mid {sm:+.1f} dB, L/R {lr:+.1f} dB')
    lin, lg = plot(L, R, size, False), plot(L, R, size, True)
    for y in range(size): rows.append(lin[y] + [60] * 4 + lg[y])
    rows.append([60] * (4 * size + 4))
png(rows, out)
print('wrote', out, '(rows: files in order; left linear radius, right dB radius)')
