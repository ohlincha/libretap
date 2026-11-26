"""A pretend network instrument for Tick's network listener tests: TCP on PORT
answers every line ending in CR with "NETTEST,<line>\\r\\n"; UDP on PORT+1 echoes
"UDPTEST,<datagram>".  Usage: python3 -I fake_net_instrument.py PORT SECONDS"""
import select
import socket
import sys
import time


def main(port, seconds):
    port = int(port)
    t = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    t.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    t.bind(("127.0.0.1", port))
    t.listen(4)
    u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    u.bind(("127.0.0.1", port + 1))
    print("fake network instrument on 127.0.0.1:%d (TCP) and :%d (UDP)" % (port, port + 1), flush=True)
    conns, bufs = [], {}
    end = time.time() + float(seconds)
    while time.time() < end:
        r, _, _ = select.select([t, u] + conns, [], [], 0.2)
        for s in r:
            if s is t:
                c, _ = t.accept()
                conns.append(c)
                bufs[c] = b""
            elif s is u:
                d, a = u.recvfrom(2048)
                u.sendto(b"UDPTEST," + d, a)
            else:
                d = s.recv(2048)
                if not d:
                    conns.remove(s)
                    s.close()
                    continue
                bufs[s] += d
                while b"\r" in bufs[s]:
                    line, bufs[s] = bufs[s].split(b"\r", 1)
                    s.sendall(b"NETTEST," + line + b"\r\n")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
