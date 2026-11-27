"""Pico firmware flashing guide (Windows, macOS, Linux)."""

from common import arrow_marker, box, line, page, pico, svg, text

URL = "https://github.com/ohlincha/libretap/raw/main/firmware/pico-usb-sniffer/bin/UsbSnifferStream.uf2"


def bootsel():
    o = [arrow_marker()]
    s = 1.45
    x0, y0 = 60, 130
    W = 210 * s
    o.append('<defs><clipPath id="top"><rect x="0" y="0" width="1000" height="' + str(y0 + 300 * s) + '"/></clipPath></defs>')
    o.append('<g clip-path="url(#top)">' + pico(x0, y0, s, label_all=True, usb_label=False) + '</g>')
    o.append(f'<path d="M {x0} {y0 + 300 * s} l 40 -12 l 40 12 l 40 -12 l 40 12 l 40 -12 l 40 12 l 40 -12 l 25 8" '
             f'fill="none" stroke="#fff" stroke-width="6"/>')
    o.append(text(x0 + W / 2, y0 + 300 * s + 30, "(upper part of the Pico, component side)", size=15, fill="#444"))
    # micro-USB cable from the top
    cx = x0 + W / 2
    o.append(f'<path d="M {cx} {y0 - 30} L {cx} 0" stroke="#333" stroke-width="14"/>')
    o.append(f'<rect x="{cx - 20}" y="{y0 - 40}" width="40" height="34" rx="4" fill="#888"/>')
    o.append(text(cx + 30, 40, "micro-USB cable to the laptop", size=16, anchor="start"))
    # finger on the button
    bx, by = x0 + W / 2, y0 + 78 * s
    o.append(f'<circle cx="{bx}" cy="{by}" r="34" fill="none" stroke="#e0662a" stroke-width="5"/>')
    o.append(line(560, by, bx + 40, by, arrow=True, width=2.5))
    o.append(text(570, by - 40, "1  Press and hold BOOTSEL.", size=21, anchor="start", weight="bold"))
    o.append(text(570, by - 4, "2  Plug in the micro-USB cable", size=21, anchor="start", weight="bold"))
    o.append(text(605, by + 24, "(laptop end already connected).", size=19, anchor="start"))
    o.append(text(570, by + 66, "3  Release BOOTSEL.", size=21, anchor="start", weight="bold"))
    o.append(text(570, by + 110, "A drive called RPI-RP2 appears", size=19, anchor="start"))
    o.append(text(570, by + 136, "on the laptop.", size=19, anchor="start"))
    o.append(text(570, by + 186, "Holding BOOTSEL while plugging in", size=16, anchor="start", fill="#444"))
    o.append(text(570, by + 208, "always opens the drive, whatever", size=16, anchor="start", fill="#444"))
    o.append(text(570, by + 230, "firmware the Pico has.", size=16, anchor="start", fill="#444"))
    return svg(1000, y0 + 300 * s + 45, "".join(o))


def drive_copy():
    o = [arrow_marker()]
    o.append('<rect x="30" y="30" width="420" height="230" rx="8" fill="#fff" stroke="#888" stroke-width="1.5"/>')
    o.append('<rect x="30" y="30" width="420" height="34" rx="8" fill="#e8ecef" stroke="#888"/>')
    o.append(text(240, 53, "Downloads", size=16, weight="bold"))
    o.append('<rect x="150" y="100" width="64" height="80" rx="4" fill="#f6f6f6" stroke="#666"/>')
    o.append(text(182, 148, "UF2", size=16, weight="bold"))
    o.append(text(182, 212, "UsbSnifferStream.uf2", size=16))
    o.append('<rect x="550" y="30" width="420" height="230" rx="8" fill="#fff" stroke="#888" stroke-width="1.5"/>')
    o.append('<rect x="550" y="30" width="420" height="34" rx="8" fill="#e8ecef" stroke="#888"/>')
    o.append(text(760, 53, "RPI-RP2", size=16, weight="bold"))
    o.append(text(650, 120, "INDEX.HTM", size=14))
    o.append(text(650, 150, "INFO_UF2.TXT", size=14))
    o.append(line(225, 140, 600, 190, arrow=True, width=3))
    o.append(text(500, 296, "Drag the file onto the RPI-RP2 drive.", size=18, weight="bold"))
    o.append(text(500, 322, "The Pico restarts by itself and the drive disappears: that is success.", size=17))
    return svg(1000, 335, "".join(o))


def done():
    o = []
    o.append(box(20, 30, 300, 110, "LED blinks slowly", "ready, not recording", size=20))
    o.append(box(350, 30, 300, 110, "LED blinks fast", "recording (streaming)", size=20))
    o.append(box(680, 30, 300, 110, "LibreTap: Connections", "USB sniffer ready (…)", size=19))
    return svg(1000, 160, "".join(o))


def build():
    b = []
    b.append(f"""<section class="first">
<h1>Pico firmware: flashing</h1>
<p class="sub">Puts LibreTap's firmware, USB Sniffer Stream, on the Raspberry Pi Pico of the USB sniffer.
Takes a minute; nothing to install. Works the same on Windows, macOS and Linux.</p>
<h3>You need</h3>
<ul>
<li>The Pico (RP2040; Pico 2 and Pico W are not supported). It may already be wired into the sniffer cable.</li>
<li>A micro-USB <b>data</b> cable. Some cables only charge: with those, no drive appears.</li>
<li>The firmware file <code>UsbSnifferStream.uf2</code>: on the LibreTap releases page
(<span class="mono">github.com/ohlincha/libretap/releases</span>, from version 0.1.3) or directly:<br>
<span class="mono">{URL}</span></li>
</ul>
<h2 style="margin-top:6mm">1. Start the Pico in its loader</h2>
<figure>{bootsel()}<figcaption>Figure 1. BOOTSEL is the only button on the Pico, near the micro-USB socket.</figcaption></figure>
</section>""")
    b.append(f"""<section>
<h2>2. Copy the file</h2>
<figure>{drive_copy()}<figcaption>Figure 2. Copying the firmware. The drive's contents may differ slightly.</figcaption></figure>
<h3>Windows (10 and 11)</h3>
<ol class="steps">
<li>The drive <b>RPI-RP2</b> opens in Explorer (otherwise open it under <i>This PC</i>).</li>
<li>Drag <code>UsbSnifferStream.uf2</code> from <i>Downloads</i> onto it.</li>
<li>The window closes as the Pico restarts. The Pico is now a serial port (COM3, COM5, …); Windows needs no driver.</li>
</ol>
<h3>macOS</h3>
<ol class="steps">
<li>RPI-RP2 appears on the desktop and in Finder's sidebar.</li>
<li>Drag <code>UsbSnifferStream.uf2</code> onto it.</li>
<li>Some macOS versions then report an error or "disk not ejected properly". If the drive has disappeared, the copy
worked; dismiss the message.</li>
<li>If Finder refuses to copy: Terminal, then <code>cp -X ~/Downloads/UsbSnifferStream.uf2 /Volumes/RPI-RP2/</code></li>
</ol>
<p>The Pico is then a serial port named <code>/dev/cu.usbmodem…</code>.</p>
<h3>Linux</h3>
<ol class="steps">
<li>The desktop mounts RPI-RP2 (in the file manager's sidebar). Drag the file onto it.</li>
<li>Or in a terminal: <code>cp ~/Downloads/UsbSnifferStream.uf2 /media/$USER/RPI-RP2/</code> (the mount point varies:
<code>/run/media/$USER/RPI-RP2</code> on some systems).</li>
<li>The Pico is then <code>/dev/ttyACM0</code> (or ACM1, …). Serial ports need the <code>dialout</code> group: if
LibreTap does not list the sniffer, run <code>sudo usermod -aG dialout $USER</code> and log out and in.</li>
</ol>
</section>""")
    b.append(f"""<section>
<h2>3. Check that it worked</h2>
<figure>{done()}<figcaption>Figure 3. Signs of the new firmware.</figcaption></figure>
<ol class="steps">
<li>Unplug and replug the Pico (without BOOTSEL). Its LED blinks slowly.</li>
<li>Start LibreTap. Connections shows <b>USB sniffer ready (Pico on …, USB Sniffer Stream). It records continuously, like
the other sources.</b></li>
</ol>
<div class="check"><b>Check.</b> The text says <i>USB Sniffer Stream</i>. If it says the sniffer "captures in short
pieces", the Pico still has the original usb-sniffer-lite firmware: flash again.</div>
<h3>Going back</h3>
<p>To return to the original usb-sniffer-lite firmware, copy its <code>UsbSnifferLite.uf2</code>
(github.com/ataradov/usb-sniffer-lite, folder <code>bin</code>) the same way. The wiring is the same for both. LibreTap
works with either; with usb-sniffer-lite it records in short pieces (Record, one action, Stop).</p>
<h2 style="margin-top:6mm">Troubleshooting</h2>
<table>
<tr><th>Symptom</th><th>Likely cause</th><th>Action</th></tr>
<tr><td>No RPI-RP2 drive appears</td><td>charge-only cable; BOOTSEL released too early</td><td>Another micro-USB cable. Hold BOOTSEL until the cable is fully in.</td></tr>
<tr><td>Drive appears but the copy fails (macOS)</td><td>Finder's copy</td><td>Terminal: <code>cp -X</code> as above.</td></tr>
<tr><td>Copy worked, the drive is back again</td><td>BOOTSEL still held, or a wrong file</td><td>Release BOOTSEL; use <code>UsbSnifferStream.uf2</code> from the link above.</td></tr>
<tr><td>LibreTap does not list the Pico (Linux)</td><td>serial port permission; ModemManager probing the port</td><td><code>dialout</code> group (above). Optional: the udev rule <code>firmware/pico-usb-sniffer/bin/90-usb-sniffer-lite.rules</code> into <code>/etc/udev/rules.d/</code>.</td></tr>
<tr><td>LED does not blink</td><td>firmware not running</td><td>Replug without BOOTSEL; flash again.</td></tr>
</table>
<p class="note">USB Sniffer Stream is untested on a real Pico so far; report problems at github.com/ohlincha/libretap/issues.</p>
<p class="footer">LibreTap — github.com/ohlincha/libretap · docs/hardware/pico-firmware-flashing · USB Sniffer Stream is a fork
of usb-sniffer-lite by Alex Taradov (BSD-3-Clause).</p>
</section>""")
    return page("Pico firmware: flashing", "", "\n".join(b))
