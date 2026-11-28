"""Raspberry Pi Pico USB sniffer build guide."""

from common import USB, arrow_marker, box, line, page, pico, pico_pin_xy, svg, text, wire
from serial_tap import joints

HL = {13: (USB["black"], "GND"), 14: (USB["green"], "D+"), 15: (USB["white"], "D-")}


def overview():
    o = [arrow_marker()]
    o.append(box(10, 40, 200, 90, "Instrument PC", "USB port"))
    o.append(box(790, 40, 200, 90, "Instrument", "its own USB cable"))
    # extension cable
    o.append('<rect x="210" y="70" width="50" height="30" rx="4" fill="#c7ccd1" stroke="#6b7177"/>')
    o.append(text(235, 150, "A plug", size=15))
    o.append('<path d="M 260 85 L 400 85" stroke="#333" stroke-width="10"/>')
    o.append('<rect x="400" y="66" width="80" height="38" rx="6" fill="#f1e3b3" stroke="#8a6d1f" stroke-width="1.5"/>')
    o.append(text(500, 60, "splice", size=15, weight="bold"))
    o.append('<path d="M 480 85 L 620 85" stroke="#333" stroke-width="10"/>')
    o.append('<rect x="620" y="65" width="56" height="40" rx="4" fill="#c7ccd1" stroke="#6b7177"/>')
    o.append(text(648, 150, "A socket", size=15))
    o.append('<path d="M 676 85 L 790 85" stroke="#555" stroke-width="7"/>')
    o.append(text(445, 30, "USB extension cable (A plug to A socket)", size=16, weight="bold"))
    # three tap wires to the Pico
    o.append(wire("M 425 104 C 425 170 300 180 290 228", USB["green"], 5))
    o.append(wire("M 440 104 C 440 175 310 185 300 228", USB["white"], 5))
    o.append(wire("M 455 104 C 455 180 330 190 315 228", USB["black"], 5))
    o.append(text(520, 170, "3 short wires: D+ (green), D- (white), GND (black)", size=15, anchor="start"))
    o.append(text(520, 192, "Red (VBUS, 5 V) is not connected to the Pico.", size=15, anchor="start"))
    o.append('<rect x="250" y="228" width="110" height="200" rx="8" fill="#1f7a4d" stroke="#0f4a2d" stroke-width="2"/>')
    o.append(text(305, 330, "Pico", size=20, weight="bold", fill="#fff"))
    o.append('<rect x="290" y="428" width="30" height="16" fill="#c7ccd1" stroke="#6b7177"/>')
    o.append(line(305, 444, 305, 492, width=4))
    o.append(text(315, 472, "micro-USB cable", size=15, anchor="start"))
    o.append(box(160, 492, 290, 62, "Laptop with LibreTap", "power and data for the Pico"))
    return svg(1000, 570, "".join(o))


def usb_contacts():
    o = []
    # A plug face, enlarged
    o.append('<rect x="60" y="40" width="380" height="150" rx="10" fill="#c7ccd1" stroke="#6b7177" stroke-width="2"/>')
    o.append('<rect x="80" y="60" width="340" height="60" rx="4" fill="#f4f4f4" stroke="#888"/>')
    xs = [110, 205, 295, 390]
    lens = [44, 34, 34, 44]
    names = [("1", "VBUS", "red", USB["red"]), ("2", "D-", "white", USB["white"]), ("3", "D+", "green", USB["green"]),
             ("4", "GND", "black", USB["black"])]
    for x, ln, (n, sig, col, fill) in zip(xs, lens, names):
        o.append(f'<rect x="{x - 22}" y="{120}" width="44" height="{ln}" fill="#d9b24a" stroke="#8a6d1f"/>')
        o.append(text(x, 216, n, size=20, weight="bold"))
        o.append(text(x, 240, sig, size=17, weight="bold"))
        o.append(f'<rect x="{x - 26}" y="252" width="52" height="16" fill="{fill}" stroke="#555"/>')
        o.append(text(x, 288, col, size=15))
    o.append(text(250, 26, "USB-A plug, contacts (enlarged)", size=17, weight="bold"))
    o.append(text(250, 312, "Outer contacts (longer): VBUS and GND. Inner: D- and D+.", size=14.5))
    # rule
    o.append(text(520, 70, "The order 1-2-3-4 runs from one outer", size=16, anchor="start"))
    o.append(text(520, 92, "contact to the other. Seen from the other", size=16, anchor="start"))
    o.append(text(520, 114, "side it reads 4-3-2-1. To be sure:", size=16, anchor="start"))
    o.append(text(520, 150, "1. Find the outer contact that beeps with", size=16, anchor="start"))
    o.append(text(540, 172, "the red wire: that is 1 (VBUS).", size=16, anchor="start"))
    o.append(text(520, 200, "2. The inner contact next to it is 2 (D-).", size=16, anchor="start"))
    o.append(text(520, 228, "3. The other inner contact is 3 (D+).", size=16, anchor="start"))
    o.append(text(520, 256, "4. The other outer contact is 4 (GND).", size=16, anchor="start"))
    return svg(1000, 325, "".join(o))


def splice_steps():
    o = []

    def jacket(x0, x1, y):
        return (f'<rect x="{x0}" y="{y - 22}" width="{x1 - x0}" height="44" rx="20" fill="#3a3a3a" stroke="#222"/>')

    # step A: window in jacket
    y = 70
    o.append(text(20, 26, "A  Remove 3 cm of outer jacket; fold the foil and braid aside.", size=17, anchor="start", weight="bold"))
    o.append(jacket(40, 360, y) + jacket(640, 960, y))
    cols = [("red", USB["red"]), ("white", USB["white"]), ("green", USB["green"]), ("black", USB["black"])]
    for i, (n, c) in enumerate(cols):
        yy = y - 15 + i * 10
        o.append(wire(f"M 340 {yy} L 660 {yy}", c, 7))
    o.append(f'<path d="M 360 {y - 26} q 30 -20 60 -6 M 640 {y - 26} q -30 -20 -60 -6" stroke="#b5b5b5" stroke-width="5" fill="none"/>')
    o.append(text(800, y - 30, "foil / braid folded back", size=15))
    o.append(text(500, y + 50, "3 cm", size=15))
    o.append(line(360, y + 34, 640, y + 34, width=1.2))

    # step B: insulation windows
    y = 210
    o.append(text(20, 150, "B  Open a 3–4 mm window in green, white and black. Do not cut the copper.",
                  size=17, anchor="start", weight="bold"))
    o.append(jacket(40, 300, y) + jacket(700, 960, y))
    ys = {"red": y - 40, "white": y - 13, "green": y + 13, "black": y + 40}
    for n, c in cols:
        yy = ys[n]
        o.append(wire(f"M 290 {y} C 360 {y} 360 {yy} 420 {yy} L 580 {yy} C 640 {yy} 640 {y} 710 {y}", c, 7))
        if n != "red":
            o.append(f'<rect x="488" y="{yy - 4}" width="24" height="8" fill="#c8823a" stroke="#7a4a14"/>')
    o.append(text(500, y + 72, "windows in white, green, black, staggered; red stays closed", size=15))

    # step C: wrap, solder, tape
    y = 375
    o.append(text(20, 312, "C  Wrap a tinned wire once around each window, solder, tape each joint.",
                  size=17, anchor="start", weight="bold"))
    o.append(jacket(40, 300, y) + jacket(700, 960, y))
    ys = {"red": y - 40, "white": y - 13, "green": y + 13, "black": y + 40}
    xs = {"white": 450, "green": 500, "black": 550}
    for n, c in cols:
        yy = ys[n]
        o.append(wire(f"M 290 {y} C 360 {y} 360 {yy} 420 {yy} L 580 {yy} C 640 {yy} 640 {y} 710 {y}", c, 7))
    for n, c in cols:
        if n == "red":
            continue
        x, yy = xs[n], ys[n]
        o.append(f'<rect x="{x - 12}" y="{yy - 8}" width="24" height="16" rx="3" fill="#2f5d9e" opacity="0.85"/>')
        o.append(wire(f"M {x} {yy + 8} L {x} {y + 90}", c, 5))
    o.append(text(570, y + 95, "to the Pico (under 10 cm); blue = tape over each joint", size=15, anchor="start"))
    return svg(1000, 490, "".join(o))


def pico_wiring():
    o = []
    s = 1.55
    x0, y0 = 250, 46
    o.append(pico(x0, y0, s, highlight=HL))
    for pin, (c, sig) in HL.items():
        x, y = pico_pin_xy(x0, y0, s, pin)
        lab = {13: "black (GND)", 14: "green (D+)", 15: "white (D-)"}[pin]
        o.append(wire(f"M {x - 12} {y} L {x - 120} {y}", c, 7))
        o.append(text(x - 128, y + 6, lab, size=19, anchor="end", weight="bold"))
    o.append(text(x0 + 163, 905, "Component side (the side with the chip). Pin 1 top left; 1–20 down the left, 21–40 up the right.", size=15))
    o.append(text(590, 150, "Solder to:", size=19, weight="bold", anchor="start"))
    o.append(text(590, 180, "pin 13 = GND", size=18, anchor="start"))
    o.append(text(590, 206, "pin 14 = GP10 (D+)", size=18, anchor="start"))
    o.append(text(590, 232, "pin 15 = GP11 (D-)", size=18, anchor="start"))
    o.append(text(590, 272, "Nothing else.", size=18, weight="bold", anchor="start"))
    o.append(text(590, 298, "Pin 16 (GP12) is", size=16, fill="#444", anchor="start"))
    o.append(text(590, 318, "used inside: leave", size=16, fill="#444", anchor="start"))
    o.append(text(590, 338, "it free.", size=16, fill="#444", anchor="start"))
    return svg(770, 920, "".join(o))


def through_hole():
    o = []
    o.append('<rect x="80" y="60" width="300" height="24" fill="#1f7a4d" stroke="#0f4a2d"/>')
    o.append('<rect x="214" y="56" width="32" height="32" fill="#f3d36b" stroke="#7a5c00"/>')
    o.append('<rect x="224" y="56" width="12" height="32" fill="#fff" stroke="#7a5c00"/>')
    o.append(wire("M 230 10 L 230 110", USB["green"], 5))
    o.append('<path d="M 214 56 Q 230 40 246 56 Z" fill="#c8cdd2" stroke="#7d848a"/>')
    o.append(text(230, 140, "wire through the hole from the", size=15))
    o.append(text(230, 160, "underside; solder on the top", size=15))
    o.append(text(560, 70, "Strip 2 mm, tin, push through,", size=15, anchor="start"))
    o.append(text(560, 92, "heat pad and wire together 2–3 s.", size=15, anchor="start"))
    o.append(text(560, 114, "Trim the end flush.", size=15, anchor="start"))
    return svg(900, 175, "".join(o))


def build():
    b = []
    b.append(f"""<section class="first">
<h1>USB sniffer: build guide</h1>
<p class="sub">A Raspberry Pi Pico spliced into a USB extension cable. It records the USB traffic between the
instrument PC and the instrument for LibreTap.</p>
<figure>{overview()}<figcaption>Figure 1. The finished sniffer. The PC and the instrument use the extension cable as
before; the Pico listens on the data lines and reports to the laptop over its own USB cable.</figcaption></figure>
<h3>Limits</h3>
<ul>
<li>Low speed (1.5 Mbit/s) and full speed (12 Mbit/s) only. Most instrument USB links are full speed. A high-speed link
(480 Mbit/s) cannot be recorded with this sniffer. On Linux, <code>lsusb -t</code> shows the speed (<code>12M</code> or
<code>1.5M</code> is fine, <code>480M</code> is not).</li>
<li>If the instrument's USB device is a USB–serial chip (FTDI, PL2303, CP210x, CH340), the serial tap on its serial
side is the simpler option.</li>
</ul>
<p class="note">Time: about 1 hour. Not yet tested on a real instrument.</p>
</section>""")
    b.append(f"""<section>
<h2>Parts and tools</h2>
<table>
<tr><th>Part</th><th>Qty</th><th>Note</th></tr>
<tr><td>Raspberry Pi Pico (RP2040)</td><td>1</td><td>Pico 2 and Pico W are not supported</td></tr>
<tr><td>USB extension cable, A plug to A socket</td><td>1</td><td>sacrificed; 1–2 m; not a "charge-only" cable</td></tr>
<tr><td>Hook-up wire, stranded, thin</td><td>3 × 10 cm</td><td>or cores from rainbow ribbon</td></tr>
<tr><td>Micro-USB cable (data)</td><td>1</td><td>Pico to laptop</td></tr>
<tr><td>Insulating tape, cable ties</td><td>—</td><td></td></tr>
</table>
<p><b>Tools:</b> soldering iron with stand, thin solder (0.5–0.8 mm), sharp craft knife, side cutters, wire stripper,
multimeter with continuity (beep) mode, something to hold the work (helping hand, tape on the bench).</p>
<p>The instrument normally plugs into the PC with an A plug. The extension sits between them: its A plug into the PC,
the instrument's cable into its A socket. If the instrument has a fixed cable with a different plug, use a sacrificial
cable of that type instead and splice it the same way.</p>
<h3>Firmware</h3>
<p>The Pico needs LibreTap's firmware, USB Sniffer Stream. Flashing takes a minute and needs no tools: see the
separate guide <i>Pico firmware: flashing</i>. Flash before or after building; the wiring does not change it.</p>
</section>""")
    b.append(f"""<section>
<h2>Soldering in brief</h2>
<ul>
<li><b>Safety.</b> Work in a ventilated room or with fume extraction. Wear eye protection (clipped wire ends fly). The
iron goes back into its stand every time you put it down. Leaded solder: wash your hands afterwards.</li>
<li><b>Solder.</b> 60/40 tin–lead is easier for beginners; lead-free works too.</li>
<li><b>Temperature.</b> 330–350 °C for leaded solder, 350–380 °C for lead-free.</li>
<li><b>Tin the tip:</b> wipe it on the damp sponge or brass wool, melt a little solder onto it.</li>
<li><b>Tin the wire</b> before joining: strip, twist the strands, coat them with solder.</li>
<li><b>Heat the joint, not the solder.</b> Tip against pad and wire, feed solder into the joint, 2–3 s, remove solder,
then iron.</li>
<li><b>Don't move it</b> until the solder has set.</li>
<li><b>Insulate every joint.</b> Heat-shrink must be slid on before soldering; on the splice it cannot go over the
uncut wires, so use tape there.</li>
</ul>
<figure class="half">{joints()}<figcaption>Figure 2. Judging a joint.</figcaption></figure>
<div class="check"><b>Practice first.</b> Solder two scrap wires together and one wire into a pad of a scrap board, if you
have one. Pull on them: a good joint holds.</div>
</section>""")
    b.append(f"""<section>
<h2>1. Identify the wires</h2>
<p>Inside a USB 2.0 cable there are four wires. The colours below are the convention, not a guarantee: confirm with the
multimeter before soldering.</p>
<figure>{usb_contacts()}<figcaption>Figure 3. USB-A contacts and the usual wire colours.</figcaption></figure>
<p>You confirm the colours in section 2, once the wires are exposed: multimeter in continuity mode, one probe on a
contact of the extension's A plug, the other on a bare wire.</p>
<div class="check"><b>Expected.</b> Contact 1 ↔ red, 2 ↔ white, 3 ↔ green, 4 ↔ black. If your cable differs, use the
measured colours from then on: what matters is <b>D+ (contact 3) → Pico pin 14, D- (contact 2) → pin 15, GND (contact 4)
→ pin 13</b>.</div>
</section>""")
    b.append(f"""<section>
<h2>2. Open the cable</h2>
<figure>{splice_steps()}<figcaption>Figure 4. Splicing without cutting: the wires stay continuous, so the PC and the
instrument are connected exactly as before.</figcaption></figure>
<ol class="steps">
<li>Choose a point about 30 cm from the A socket end. Score the outer jacket around the cable twice, 3 cm apart, and
once lengthwise between the two. Do not press hard: the wires lie just underneath.</li>
<li>Peel the jacket off. Fold the foil and the braid (if any) back, or cut them off over the opening only (harmless at
these speeds). A bare wire in the foil is the drain, part of the shield, not GND. The shield is never connected to the
Pico.</li>
<li>Spread the four wires. On <b>green</b>, <b>white</b> and <b>black</b>, cut two shallow rings 3–4 mm apart and peel
the insulation off between them. Stagger the three windows so the joints are not side by side.</li>
<li>Leave <b>red</b> closed.</li>
<li>Confirm the colours (section 1): probe each contact of the A plug against the windows.</li>
</ol>
<div class="check"><b>Check.</b> Each window shows bright copper; no strands are cut. Continuity from the A plug through to
the A socket is still there for all four contacts (contact n of the plug to contact n of the socket).</div>
<div class="warn"><b>If a wire breaks.</b> These cores are thin and snap easily. <b>Black (GND)</b>: strip both ends 5 mm and
twist them together with the Pico's black wire into one joint; solder and tape it. That joint replaces the black window.
<b>Several wires damaged, or windows too fiddly</b>: cut the cable through. Join each colour straight through, adding the
Pico's wire into the green, white and black joints (three wires per joint; red is only rejoined). Slide heat-shrink on
before soldering, stagger the joints, keep green and white untwisted for at most 2–3 cm, then twist them loosely again.
If the shield was cut, join the braid of both sides with a short wire (optional). Then repeat the check above, and check
that no two wires beep with each other.</div>
</section>""")
    b.append(f"""<section>
<h2>3. Solder the wires to the cable</h2>
<ol class="steps">
<li>Cut three wires, 10 cm each (green, white, black if you have them; otherwise label them).</li>
<li>Strip 1 cm at one end, tin it, and wrap it once around the matching copper window: green on green, white on white,
black on black.</li>
<li>Solder each wrap (2–3 s). Let it cool.</li>
<li>Wrap each joint separately with tape, then tape all four cable wires together, and the jacket ends over them.</li>
<li>Twist the green and white wires loosely together (about one turn per 2 cm).</li>
<li>Fix the bundle with a cable tie on each side of the opening so no pull reaches the joints.</li>
</ol>
<div class="check"><b>Check.</b> No bare copper visible. Green wire end beeps with contact 3 of the A plug, white with 2, black
with 4. None of the three beeps with contact 1 (red, 5 V).</div>
</section>""")
    b.append(f"""<section>
<h2>4. Solder the wires to the Pico</h2>
<figure>{pico_wiring()}<figcaption>Figure 5. Raspberry Pi Pico from the component side. Only pins 13, 14 and 15 are used.</figcaption></figure>
</section>""")
    b.append(f"""<section>
<h2>4. Solder the wires to the Pico (continued)</h2>
<figure>{through_hole()}<figcaption>Figure 6. Soldering a wire into a Pico pad.</figcaption></figure>
<ol class="steps">
<li>Lay the Pico component side up, micro-USB at the top. Count pins down the left edge from the top: 1, 2, 3 …
Find pin 13. Many Picos have the pin names printed on the underside: if yours does, confirm GND, GP10 and GP11
there.</li>
<li>Shorten the three wires to the length you need (under 10 cm from the cable). Strip 2 mm and tin.</li>
<li>Black (GND) into pin 13, from the underside; solder on the component side.</li>
<li>Green (D+) into pin 14.</li>
<li>White (D-) into pin 15.</li>
<li>Trim the ends flush.</li>
</ol>
<div class="check"><b>Check.</b> Pin 13 beeps with contact 4 of the A plug (GND), pin 14 with contact 3 (D+), pin 15 with
contact 2 (D-). No beep between pins 13, 14, 15 and 16, or between any of them and contact 1. The red wire is not
connected to the Pico.</div>
</section>""")
    b.append(f"""<section>
<h2>5. Test and use</h2>
<ol class="steps">
<li>Plug the extension into a computer and a USB mouse into its socket. The mouse must work as before. If not, a
splice is broken or bridged: recheck section 2.</li>
<li>Flash the firmware (separate guide) if not done yet.</li>
<li>At the instrument: close the vendor software, unplug the instrument's USB cable from the PC, plug the extension into
the PC and the instrument's cable into the extension.</li>
<li>Connect the Pico to the laptop with the micro-USB cable. LibreTap's Connections shows <b>USB sniffer ready (Pico on
…, USB Sniffer Stream)</b>.</li>
<li>Low-speed device (1.5 Mbit/s: keyboards, some old devices)? Tick that in LibreTap ▸ Advanced…. Instruments are
almost always full speed: leave it unticked.</li>
<li>Start the vendor software; it must work as before. Press Record in LibreTap.</li>
</ol>
<h2 style="margin-top:6mm">Troubleshooting</h2>
<table>
<tr><th>Symptom</th><th>Likely cause</th><th>Action</th></tr>
<tr><td>Instrument no longer found by the PC</td><td>broken copper in a window, or two windows touching</td><td>Continuity contact n ↔ n through the cable; no beep between neighbours. Retape.</td></tr>
<tr><td>Pico not listed in LibreTap</td><td>firmware missing, charge-only micro-USB cable, or (Linux) permission</td><td>Flashing guide. Try another cable. Linux: <code>sudo usermod -aG dialout $USER</code>, log out and in.</td></tr>
<tr><td>Recording shows errors ("wrong speed setting, or a poor splice")</td><td>speed setting, swapped D+/D-, long or untwisted tap wires</td><td>Check the low-speed setting; check pin 14 = D+ (contact 3); shorten and twist the wires.</td></tr>
<tr><td>Nothing recorded while the software works</td><td>high-speed link (480 Mbit/s), or the Pico is on a different cable</td><td><code>lsusb -t</code> on a Linux PC with the instrument. 480M cannot be recorded.</td></tr>
<tr><td>Pico gets warm, PC reports a USB power problem</td><td>red (5 V) connected to something</td><td>Unplug. Red must not touch any other wire or the Pico.</td></tr>
</table>
<p class="footer">LibreTap — github.com/ohlincha/libretap · docs/hardware/usb-sniffer-build · MIT licence.
Firmware: USB Sniffer Stream, a fork of usb-sniffer-lite by Alex Taradov (BSD-3-Clause).</p>
</section>""")
    return page("USB sniffer: build guide", "", "\n".join(b))
