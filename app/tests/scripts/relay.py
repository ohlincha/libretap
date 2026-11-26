#!/usr/bin/env python3
"""Serial relay for LibreTap's end-to-end tests: a pretend instrument on a pseudo-terminal (the vendor program's
COM port; LINK is a symlink to it, e.g. for Wine's COM1), answering every line ending in CR with
"TICKTEST,<line>\\r\\n", and two more pseudo-terminals that carry a copy of each direction (the serial tap's two
leads). Prints "A <path>" and "B <path>" for the tap, then runs for SECONDS.
Usage: python3 -I relay.py LINK SECONDS"""
import os
import pty
import select
import sys
import time
import tty


def opened():
    m, s = pty.openpty()
    tty.setraw(s)
    return m, s, os.ttyname(s)


def main(link, seconds):
    vm, vs, vname = opened()
    am, as_, aname = opened()
    bm, bs, bname = opened()
    if os.path.lexists(link):
        os.remove(link)
    os.symlink(vname, link)
    print("A %s\nB %s" % (aname, bname), flush=True)
    end = time.time() + float(seconds)
    line = b""
    while time.time() < end:
        r, _, _ = select.select([vm], [], [], 0.1)
        if not r:
            continue
        try:
            data = os.read(vm, 1024)
        except OSError:
            continue
        os.write(am, data)
        line += data
        while b"\r" in line:
            q, line = line.split(b"\r", 1)
            time.sleep(0.008)
            ans = b"TICKTEST," + q + b"\r\n"
            os.write(vm, ans)
            os.write(bm, ans)
    os.remove(link)


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
