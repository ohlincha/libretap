"""Ethernet capture: setting up a port-mirroring switch (TP-Link TL-SG105E)."""

from common import arrow_marker, box, line, page, svg, text


def cabling():
    o = [arrow_marker()]
    # switch with five ports
    sx, sy, sw, sh = 250, 200, 500, 120
    o.append(f'<rect x="{sx}" y="{sy}" width="{sw}" height="{sh}" rx="10" fill="#e8edf0" stroke="#6b7177" stroke-width="1.8"/>')
    o.append(text(sx + 295, sy + sh - 8, "TL-SG105E, port mirroring", size=15, weight="bold", fill="#24524e"))
    px = {}
    for i in range(5):
        x = sx + 60 + i * 95
        px[i + 1] = x
        used = i + 1 in (1, 2, 5)
        o.append(f'<rect x="{x - 26}" y="{sy + 50}" width="52" height="44" rx="4" '
                 f'fill="{"#5c6670" if used else "#cfd5da"}" stroke="#333"/>')
        o.append(text(x, sy + 79, str(i + 1), size=19, weight="bold", fill="#fff" if used else "#666"))
    # devices
    o.append(box(20, 20, 250, 90, "Instrument", "e.g. Vertex 80v bench"))
    o.append(box(330, 20, 250, 90, "Instrument PC", "vendor software (OPUS …)"))
    o.append(box(690, 20, 290, 90, "Laptop with LibreTap", "listens only"))
    cab = 'stroke="#2f6f6a" stroke-width="6" fill="none" stroke-linecap="round"'
    o.append(f'<path d="M 145 110 C 145 170 {px[1]} 160 {px[1]} {sy + 50}" {cab}/>')
    o.append(f'<path d="M 455 110 C 455 170 {px[2]} 160 {px[2]} {sy + 50}" {cab}/>')
    o.append(f'<path d="M 835 110 C 835 170 {px[5]} 160 {px[5]} {sy + 50}" {cab}/>')
    # what the mirror does
    y = sy + sh + 40
    o.append(line(px[1], sy + 94, px[1], y, dash="5,4", width=2))
    o.append(line(px[2], sy + 94, px[2], y, dash="5,4", width=2))
    o.append(line(px[1], y, px[5], y, dash="5,4", width=2))
    o.append(line(px[5], y, px[5], sy + 96, dash="5,4", width=2.4, arrow=True))
    o.append(text((px[2] + px[5]) / 2, y + 26,
                  "copies of everything on ports 1 and 2 (both directions) go to port 5", size=16, fill="#444"))
    return svg(1000, y + 40, "".join(o))


def build():
    b = [f"""<section class="first">
<h1>Ethernet capture: switch setup</h1>
<p class="sub">Lets LibreTap see the network traffic between an instrument and its PC, using a switch with port
mirroring (TP-Link TL-SG105E). The instrument and the PC notice nothing; the laptop only listens.</p>
<figure>{cabling()}<figcaption>Figure 1. Cabling. Ports 3 and 4 stay empty.</figcaption></figure>
<p>An ordinary switch sends the laptop only its own traffic and broadcasts, so plugging the laptop into the lab
network shows nothing useful. A switch with <b>port mirroring</b> copies the traffic of chosen ports to one more
port. Unmanaged switches (most office switches, e.g. TL-SG1008D) cannot do this.</p>
<h3>You need</h3>
<ul>
<li>TP-Link TL-SG105E (or another switch with port mirroring) and its power adapter.</li>
<li>Three network cables: instrument, instrument PC, laptop.</li>
<li>A laptop with an Ethernet port (or a USB–Ethernet adapter) and LibreTap, which uses Wireshark's dumpcap.
Wireshark or tcpdump alone also work.</li>
</ul>

<h2 style="margin-top:6mm">1. Set up the switch (once)</h2>
<ol class="steps">
<li>Power the switch and connect <b>only the laptop</b> to it. The switch's address is <b>192.168.0.1</b>.</li>
<li>Give the laptop's Ethernet port the fixed address <b>192.168.0.10</b>, mask 255.255.255.0, no gateway.<br>
Windows: Settings ▸ Network ▸ Ethernet ▸ IP assignment ▸ Edit ▸ Manual, IPv4.<br>
macOS: System Settings ▸ Network ▸ Ethernet ▸ Details ▸ TCP/IP ▸ Configure IPv4: Manually.<br>
Linux: <code>sudo ip addr add 192.168.0.10/24 dev &lt;interface&gt;</code></li>
<li>Open <code>http://192.168.0.1</code> in a browser and log in as <b>admin</b> / <b>admin</b>. Set a new password
when asked and write it on the switch.</li>
<li><b>Monitoring ▸ Port Mirror</b>: enable mirroring. <b>Mirroring port</b> = 5 (the laptop). <b>Mirrored
ports</b> = 1 and 2, each with <b>Ingress</b> and <b>Egress</b>. Apply.</li>
<li><b>System ▸ Save Config</b>. Without this the setting is lost when the switch loses power.</li>
</ol>
<p>The laptop's fixed address is only needed for this; set it back to automatic afterwards if you like.</p>
</section>""", """<section>
<h2>2. Cable it</h2>
<table>
<tr><th>Port</th><th>Connect</th><th>Role</th></tr>
<tr><td>1</td><td>Instrument</td><td>mirrored (ingress + egress)</td></tr>
<tr><td>2</td><td>Instrument PC</td><td>mirrored (ingress + egress)</td></tr>
<tr><td>3, 4</td><td>nothing</td><td>—</td></tr>
<tr><td>5</td><td>Laptop</td><td>mirroring port: receives the copies</td></tr>
</table>
<ol class="steps">
<li>Unplug the cable between the instrument and its PC and put the switch in between (Figure 1). If the
instrument and PC are on the lab network through another switch, plug that switch's cable into port 3 instead
of leaving it empty; it is then not mirrored.</li>
<li>Check that the vendor software still connects to the instrument. Instrument and PC keep their own addresses
and settings; nothing changes on the instrument PC.</li>
</ol>

<h2 style="margin-top:6mm">3. Capture</h2>
<p><b>LibreTap</b>: tick <b>Ethernet</b> under Connections, choose the laptop's Ethernet port, then Record. Note
what you do in the vendor software as you go (marks), so the traffic can be matched to the screen.</p>
<p><b>Without LibreTap</b>: Wireshark on the Ethernet port, saved as pcapng; or on Linux and macOS:</p>
<pre>sudo tcpdump -i &lt;interface&gt; -s 0 -w eth_$(date +%Y%m%d_%H%M%S).pcap</pre>
<div class="check"><b>Check.</b> You see packets between <b>two addresses that are not the laptop's</b>: the
instrument and its PC.</div>

<h2 style="margin-top:6mm">Troubleshooting</h2>
<table>
<tr><th>Symptom</th><th>Likely cause</th><th>Action</th></tr>
<tr><td>Only broadcasts (ARP, DHCP, NetBIOS, mDNS)</td><td>mirroring off, or not saved and the switch was unplugged</td><td>Repeat steps 4 and 5 of part 1.</td></tr>
<tr><td>Nothing at all</td><td>wrong interface; laptop not in port 5</td><td>Pick the Ethernet port in LibreTap; check the cable is in port 5.</td></tr>
<tr><td>Cannot open http://192.168.0.1 again</td><td>laptop has no 192.168.0.x address; other devices plugged in</td><td>Laptop alone on the switch, address 192.168.0.10 (part 1, step 2).</td></tr>
</table>
<p class="note">Password lost: hold the switch's Reset button about 5 s with power on (back to admin / admin, no
mirroring). Menu names are from TP-Link's documentation and not yet checked on a real TL-SG105E; other firmware
versions may name them differently. Untested on real instruments.</p>
<p class="footer">LibreTap — github.com/ohlincha/libretap · docs/hardware/switch-setup</p>
</section>"""]
    return page("Ethernet capture: switch setup", "", "\n".join(b))
