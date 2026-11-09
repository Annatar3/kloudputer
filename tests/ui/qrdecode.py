# Independent QR reader for the preview PNGs (no QR library on this laptop): finds the symbol, reads
# format info, unmasks, collects codewords in the standard zigzag order, checks the Reed-Solomon
# codewords against a fresh computation, and decodes byte/alphanumeric segments. Versions 1-4, one block.
# Usage: qrdecode.py image.png expected-text
import sys
from PIL import Image

DATA_CW = {('L', 1): 19, ('L', 2): 34, ('L', 3): 55, ('L', 4): 80, ('M', 1): 16, ('M', 2): 28, ('M', 3): 44}
ECC_CW = {('L', 1): 7, ('L', 2): 10, ('L', 3): 15, ('L', 4): 20, ('M', 1): 10, ('M', 2): 16, ('M', 3): 26}
ALIGN = {1: [], 2: [6, 18], 3: [6, 22], 4: [6, 26]}
ECL = {1: 'L', 0: 'M', 3: 'Q', 2: 'H'}
ALNUM = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ $%*+-./:"

def grid(path):
    img = Image.open(path).convert('RGB')
    w, h = img.size
    px = img.load()
    dark = [(x, y) for y in range(h) for x in range(w) if px[x, y] == (0, 0, 0)]
    x0, y0 = min(x for x, _ in dark), min(y for _, y in dark)
    x1 = max(x for x, _ in dark)
    run = 0
    while px[x0 + run, y0] == (0, 0, 0):
        run += 1
    m = run // 7  # the finder's top edge is 7 modules of dark
    n = (x1 - x0 + 1) // m
    return [[1 if px[x0 + c * m + m // 2, y0 + r * m + m // 2] == (0, 0, 0) else 0 for c in range(n)] for r in range(n)]

def gf_tables():
    exp, log = [0] * 512, [0] * 256
    v = 1
    for i in range(255):
        exp[i], log[v] = v, i
        v <<= 1
        if v & 0x100:
            v ^= 0x11D
    for i in range(255, 512):
        exp[i] = exp[i - 255]
    return exp, log

def rs_ecc(data, k):
    exp, log = gf_tables()
    gen = [1]
    for i in range(k):
        nxt = [0] * (len(gen) + 1)
        for j, g in enumerate(gen):
            nxt[j] ^= g
            if g:
                nxt[j + 1] ^= exp[log[g] + i]
        gen = nxt
    rem = list(data) + [0] * k
    for i in range(len(data)):
        f = rem[i]
        if f:
            for j in range(1, len(gen)):
                if gen[j]:
                    rem[i + j] ^= exp[log[f] + log[gen[j]]]
    return rem[len(data):]

def decode(g):
    n = len(g)
    ver = (n - 17) // 4
    bits = [g[i][8] for i in range(6)] + [g[7][8], g[8][8], g[8][7]] + [g[8][14 - i] for i in range(9, 15)]
    fmt = sum(b << i for i, b in enumerate(bits)) ^ 0x5412
    data5 = fmt >> 10
    rem = data5
    for _ in range(10):
        rem = (rem << 1) ^ ((rem >> 9) * 0x537)
    assert (data5 << 10 | rem) == fmt, "format bits fail their BCH check"
    ecl, mask = ECL[data5 >> 3], data5 & 7
    fn = [[False] * n for _ in range(n)]
    def block(x0, y0, x1, y1):
        for y in range(y0, y1):
            for x in range(x0, x1):
                fn[y][x] = True
    block(0, 0, 9, 9); block(n - 8, 0, n, 9); block(0, n - 8, 9, n)
    block(6, 0, 7, n); block(0, 6, n, 7)
    for cy in ALIGN[ver]:
        for cx in ALIGN[ver]:
            if not fn[cy][cx]:
                block(cx - 2, cy - 2, cx + 3, cy + 3)
    masks = [lambda x, y: (x + y) % 2 == 0, lambda x, y: y % 2 == 0, lambda x, y: x % 3 == 0,
             lambda x, y: (x + y) % 3 == 0, lambda x, y: (x // 3 + y // 2) % 2 == 0,
             lambda x, y: x * y % 2 + x * y % 3 == 0, lambda x, y: (x * y % 2 + x * y % 3) % 2 == 0,
             lambda x, y: ((x + y) % 2 + x * y % 3) % 2 == 0]
    stream = []
    right = n - 1
    while right >= 1:
        if right == 6:
            right = 5
        for vert in range(n):
            for j in range(2):
                x = right - j
                y = n - 1 - vert if ((right + 1) & 2) == 0 else vert
                if not fn[y][x]:
                    stream.append(g[y][x] ^ (1 if masks[mask](x, y) else 0))
        right -= 2
    k, e = DATA_CW[(ecl, ver)], ECC_CW[(ecl, ver)]
    cw = [int(''.join(map(str, stream[i * 8:i * 8 + 8])), 2) for i in range(k + e)]
    assert rs_ecc(cw[:k], e) == cw[k:], "Reed-Solomon codewords don't match the data"
    bits = ''.join(format(c, '08b') for c in cw[:k])
    pos, out = 0, ''
    while pos + 4 <= len(bits):
        mode = bits[pos:pos + 4]; pos += 4
        if mode == '0100':
            count = int(bits[pos:pos + 8], 2); pos += 8
            out += bytes(int(bits[pos + 8 * i:pos + 8 * i + 8], 2) for i in range(count)).decode('latin-1'); pos += 8 * count
        elif mode == '0010':
            count = int(bits[pos:pos + 9], 2); pos += 9
            for _ in range(count // 2):
                v = int(bits[pos:pos + 11], 2); pos += 11
                out += ALNUM[v // 45] + ALNUM[v % 45]
            if count % 2:
                out += ALNUM[int(bits[pos:pos + 6], 2)]; pos += 6
        else:
            break
    return out, ver, ecl, mask

text, ver, ecl, mask = decode(grid(sys.argv[1]))
ok = text == sys.argv[2]
print(("ok  " if ok else "FAIL") + f"  {sys.argv[1]}: version {ver}-{ecl} mask {mask}, RS ok, reads {text!r}")
sys.exit(0 if ok else 1)
