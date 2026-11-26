"""A pretend instrument on a pseudo-terminal, for Wine tests of Tick's
listener: answers every line ending in CR with "TICKTEST,<line>\\r\\n".
Creates the pty, symlinks LINK to it (any path; the Wine test points
HKLM\\Software\\Wine\\Ports COM1 at that link, since Wine rewrites
dosdevices/com1 itself), and runs for SECONDS.
Usage: python3 -I fake_instrument.py LINK SECONDS"""
import os
import pty
import select
import sys
import termios
import time
import tty


def main(link, seconds):
    master, slave = pty.openpty()
    tty.setraw(slave)
    name = os.ttyname(slave)
    if os.path.lexists(link):
        os.remove(link)
    os.symlink(name, link)
    print("fake instrument on %s (%s)" % (name, link), flush=True)
    end = time.time() + float(seconds)
    buf = b""
    while time.time() < end:
        r, _, _ = select.select([master], [], [], 0.2)
        if not r:
            continue
        try:
            data = os.read(master, 1024)
        except OSError:
            continue
        buf += data
        if os.environ.get("FAKE_VERBOSE"):
            print("got %r" % data, flush=True)
        while b"\r" in buf:
            line, buf = buf.split(b"\r", 1)
            os.write(master, b"TICKTEST," + line + b"\r\n")
    termios.tcflush(master, termios.TCIOFLUSH)
    os.remove(link)


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
