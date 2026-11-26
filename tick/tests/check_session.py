"""Read a Tick session folder the way LibreTap will (docs/FORMAT.md) and check it:
every events.jsonl line is JSON with t and kind (a final partial line is allowed),
times never go backwards by more than 1 s, every frame file exists and decodes to
the size in its event, regions lie inside the screen, the first frame is full.
Rebuilds the screen after the last frame and writes it to OUT.png if given.
Usage: python3 -I check_session.py SESSION_DIR [OUT.png]"""
import json
import os
import struct
import sys
import zlib


def read_png(data):
    assert data[:8] == b"\x89PNG\r\n\x1a\n"
    pos, idat, ihdr = 8, b"", None
    while pos < len(data):
        n, = struct.unpack(">I", data[pos:pos + 4])
        typ, body = data[pos + 4:pos + 8], data[pos + 8:pos + 8 + n]
        assert zlib.crc32(typ + body) == struct.unpack(">I", data[pos + 8 + n:pos + 12 + n])[0]
        if typ == b"IHDR":
            ihdr = struct.unpack(">IIBBBBB", body)
        elif typ == b"IDAT":
            idat += body
        elif typ == b"IEND":
            break
        pos += 12 + n
    w, h = ihdr[0], ihdr[1]
    raw, stride = zlib.decompress(idat), w * 3
    rows, prev = [], bytearray(stride)
    for y in range(h):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        if f == 1:
            for i in range(3, stride):
                line[i] = (line[i] + line[i - 3]) & 255
        elif f == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 255
        rows.append(line)
        prev = line
    return w, h, rows


def write_png(path, w, h, rows):
    raw = b"".join(b"\0" + bytes(r) for r in rows)

    def chunk(t, b):
        return struct.pack(">I", len(b)) + t + b + struct.pack(">I", zlib.crc32(t + b))
    open(path, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                           + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


def main(d, out=None):
    meta = json.load(open(os.path.join(d, "session.json"), encoding="utf-8"))
    assert meta["format"] == "libretap-session" and meta["version"] == 1
    screen = [s for s in meta["sources"] if s["id"] == "screen"]
    lines = open(os.path.join(d, "events.jsonl"), "rb").read().split(b"\n")
    events, partial = [], 0
    for i, ln in enumerate(lines):
        if not ln:
            continue
        try:
            events.append(json.loads(ln.decode("utf-8")))
        except ValueError:
            assert i == len(lines) - 1, "bad line %d" % (i + 1)
            partial = 1
    kinds = {}
    last_t, canvas, W, H, nframes = 0, None, 0, 0, 0
    for e in events:
        assert isinstance(e["t"], float) and isinstance(e["kind"], str)
        assert e["t"] > last_t - 1.0, "time went back at %r" % e
        last_t = max(last_t, e["t"])
        kinds[e["kind"]] = kinds.get(e["kind"], 0) + 1
        if e["kind"] != "frame":
            continue
        w, h, rows = read_png(open(os.path.join(d, e["file"]), "rb").read())
        assert (w, h) == (e["w"], e["h"]), e
        if canvas is None:
            assert e["full"], "first frame must be full"
        if e["full"]:
            W, H = w, h
            canvas = [bytearray(r) for r in rows]
        else:
            assert e["x"] + w <= W and e["y"] + h <= H, e
            for y in range(h):
                canvas[e["y"] + y][e["x"] * 3:(e["x"] + w) * 3] = rows[y]
        nframes += 1
    if screen and canvas is not None:
        assert (W, H) == (screen[0]["width"], screen[0]["height"])
    size = sum(os.path.getsize(os.path.join(r, f)) for r, _, fs in os.walk(d) for f in fs)
    print("%s: %d events %s%s, %d frame files, %.1f kB, %.1f s" % (
        os.path.basename(d.rstrip("/")), len(events), json.dumps(kinds, sort_keys=True),
        " + partial last line" if partial else "", nframes, size / 1024.0,
        events[-1]["t"] - events[0]["t"] if events else 0))
    if out and canvas is not None:
        write_png(out, W, H, canvas)
        print("screen after the last frame: %s" % out)
    return 0


if __name__ == "__main__":
    sys.exit(main(*sys.argv[1:]))
