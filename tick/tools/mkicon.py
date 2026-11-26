"""Draw Tick's icon (a clock face with a red record dot) as a 16-colour .ico
that every Windows version from 95 on can show.  Usage: mkicon.py OUT.ico"""
import struct
import sys

# standard 16-colour VGA palette (B, G, R, 0)
PAL = [(0, 0, 0), (128, 0, 0), (0, 128, 0), (128, 128, 0), (0, 0, 128), (128, 0, 128), (0, 128, 128),
       (192, 192, 192), (128, 128, 128), (255, 0, 0), (0, 255, 0), (255, 255, 0), (0, 0, 255),
       (255, 0, 255), (0, 255, 255), (255, 255, 255)]  # as R, G, B
BLACK, DGREY, WHITE, RED, NAVY = 0, 8, 15, 9, 4


def draw(n):
    px = [[None] * n for _ in range(n)]
    c = (n - 1) / 2.0
    r = n / 2.0 - 0.5
    for y in range(n):
        for x in range(n):
            d = ((x - c) ** 2 + (y - c) ** 2) ** 0.5
            if d <= r - n / 16.0:
                px[y][x] = WHITE
            elif d <= r:
                px[y][x] = NAVY
    # hands: to 12 and to 3
    w = max(1, n // 16)
    for y in range(int(c - r * 0.7), int(c) + 1):
        for k in range(w):
            px[y][int(c) + k - w // 2] = BLACK
    for x in range(int(c), int(c + r * 0.55) + 1):
        for k in range(w):
            px[int(c) + k - w // 2][x] = BLACK
    # record dot, lower left
    rr = n / 6.0
    cx, cy = n * 0.27, n * 0.73
    for y in range(n):
        for x in range(n):
            if ((x - cx) ** 2 + (y - cy) ** 2) ** 0.5 <= rr:
                px[y][x] = RED
    return px


def bmp(n):
    px = draw(n)
    hdr = struct.pack("<IiiHHIIiiII", 40, n, 2 * n, 1, 4, 0, 0, 0, 0, 16, 0)
    pal = b"".join(struct.pack("<BBBB", b, g, r, 0) for (r, g, b) in PAL)
    xor = b""
    rowb = ((n * 4 + 31) // 32) * 4
    for y in reversed(range(n)):
        row = bytearray(rowb)
        for x in range(n):
            v = px[y][x] or 0
            row[x // 2] |= v << (4 if x % 2 == 0 else 0)
        xor += bytes(row)
    andb = b""
    rowa = ((n + 31) // 32) * 4
    for y in reversed(range(n)):
        row = bytearray(rowa)
        for x in range(n):
            if px[y][x] is None:
                row[x // 8] |= 0x80 >> (x % 8)
        andb += bytes(row)
    return hdr + pal + xor + andb


def main(out):
    imgs = [(32, bmp(32)), (16, bmp(16))]
    data = struct.pack("<HHH", 0, 1, len(imgs))
    off = 6 + 16 * len(imgs)
    for n, b in imgs:
        data += struct.pack("<BBBBHHII", n, n, 16, 0, 1, 4, len(b), off)
        off += len(b)
    data += b"".join(b for _, b in imgs)
    open(out, "wb").write(data)


if __name__ == "__main__":
    main(sys.argv[1])
