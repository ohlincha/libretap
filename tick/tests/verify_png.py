"""Check the files written by test_core: zlib streams inflate to the input,
PNGs decode (own minimal PNG reader on top of Python's zlib) to the
expected RGB pixels.  Run with python3 -I tests/verify_png.py OUTDIR."""
import os
import struct
import sys
import zlib


def read_png(data):
    assert data[:8] == b"\x89PNG\r\n\x1a\n", "signature"
    pos, idat, ihdr = 8, b"", None
    while pos < len(data):
        n, = struct.unpack(">I", data[pos:pos + 4])
        typ = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + n]
        crc, = struct.unpack(">I", data[pos + 8 + n:pos + 12 + n])
        assert zlib.crc32(typ + body) == crc, "crc of %r" % typ
        if typ == b"IHDR":
            ihdr = struct.unpack(">IIBBBBB", body)
        elif typ == b"IDAT":
            idat += body
        elif typ == b"IEND":
            break
        pos += 12 + n
    w, h, depth, ctype, _, _, _ = ihdr
    assert depth == 8 and ctype == 2
    raw = zlib.decompress(idat)
    stride = w * 3
    assert len(raw) == h * (stride + 1)
    out = bytearray()
    prev = bytearray(stride)
    for y in range(h):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        if f == 1:
            for i in range(3, stride):
                line[i] = (line[i] + line[i - 3]) & 255
        elif f == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 255
        else:
            assert f == 0, "filter %d" % f
        out += line
        prev = line
    return w, h, bytes(out)


def main(d):
    bad = 0
    for name in sorted(os.listdir(d)):
        p = os.path.join(d, name)
        if name.endswith(".zlib"):
            src = open(p[:-5] + ".bin", "rb").read()
            z = open(p, "rb").read()
            ok = zlib.decompress(z) == src
            print("%-10s %8d -> %8d %s" % (name, len(src), len(z), "ok" if ok else "MISMATCH"))
            bad += not ok
        elif name.endswith(".png"):
            w, h, rgb = read_png(open(p, "rb").read())
            ok = rgb == open(p[:-4] + ".rgb", "rb").read()
            print("%-10s %dx%d %d bytes %s" % (name, w, h, os.path.getsize(p), "ok" if ok else "MISMATCH"))
            bad += not ok
    print("verify: %s" % ("all ok" if not bad else "%d bad" % bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
