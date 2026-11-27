"""DB-9 serial tap build guide."""

from common import (RIBBON, arrow_marker, box, db9, legend_ribbon, line, page, svg, text, wire)

C = {p: RIBBON[p][1] for p in RIBBON}


def overview():
    o = [arrow_marker()]
    o.append(box(8, 70, 178, 90, "Instrument PC", "COM port: male DB-9"))
    o.append(box(822, 70, 170, 90, "Instrument", "e.g. IFS 66v/S"))
    # tap female end
    o.append('<rect x="200" y="85" width="70" height="60" rx="8" fill="#5c6670" stroke="#333"/>')
    o.append(text(235, 120, "F", size=22, weight="bold", fill="#fff"))
    o.append(text(235, 66, "tap, female end", size=16, weight="bold"))
    o.append(line(186, 115, 200, 115, width=3))
    # ribbon pass-through
    for i, p in enumerate(range(1, 10)):
        y = 96 + i * 4.2
        o.append(f'<line x1="270" y1="{y}" x2="460" y2="{y}" stroke="{C[p]}" stroke-width="3.6"/>')
    o.append('<rect x="270" y="94" width="190" height="40" fill="none" stroke="#777" stroke-width="0.8"/>')
    o.append(text(365, 36, "pass-through: 9 wires, 15 cm", size=16))
    o.append('<rect x="460" y="85" width="70" height="60" rx="8" fill="#5c6670" stroke="#333"/>')
    o.append(text(495, 120, "M", size=22, weight="bold", fill="#fff"))
    o.append(text(495, 66, "tap, male end", size=16, weight="bold"))
    # instrument cable
    o.append('<path d="M 530 115 C 640 115 700 115 830 115" stroke="#444" stroke-width="7" fill="none"/>')
    o.append(text(680, 104, "the instrument's own cable", size=16))
    # leads X and Y
    o.append(wire("M 225 145 C 225 230 150 260 115 300", C[3], 5))
    o.append(wire("M 233 145 C 233 232 158 262 125 302", C[4], 5))
    o.append(wire("M 245 145 C 250 230 360 260 395 300", C[2], 5))
    o.append(wire("M 253 145 C 258 230 370 262 405 302", C[1], 5))
    for x, lab, sub in ((80, "X", "what the PC sends"), (360, "Y", "what the instrument sends")):
        o.append(f'<rect x="{x}" y="300" width="80" height="50" rx="8" fill="#5c6670" stroke="#333"/>')
        o.append(text(x + 40, 333, f"{lab}  (F)", size=19, weight="bold", fill="#fff"))
        o.append(text(x + 40, 372, f"lead {lab}: {sub}", size=16))
        o.append(f'<rect x="{x - 20}" y="386" width="120" height="48" rx="6" fill="#e8edf0" stroke="#6b7177"/>')
        o.append(text(x + 40, 406, "USB–RS-232", size=14))
        o.append(text(x + 40, 424, "adapter (M)", size=14))
        o.append(line(x + 40, 434, x + 40, 470, width=3))
    o.append(box(40, 470, 440, 66, "Laptop with LibreTap", "listens only; it cannot send"))
    o.append(text(500, 440, "Lead X: orange + yellow (from tap pins 3 and 5)", size=17, anchor="start"))
    o.append(text(500, 466, "Lead Y: red + brown (from tap pins 2 and 5)", size=17, anchor="start"))
    o.append(text(500, 492, "Plugs X and Y: pins 2 and 5 only.", size=17, anchor="start"))
    o.append(text(500, 518, "F = female, M = male connector.", size=17, anchor="start"))
    return svg(1000, 550, "".join(o))


def pin_views():
    o = []
    g1, _ = db9(250, 150, 2.2, "female", "Female connector, solder side",
                colours={p: C[p] for p in range(1, 10)})
    g2, _ = db9(750, 150, 2.2, "male", "Male connector, solder side",
                colours={p: C[p] for p in range(1, 10)})
    o += [g1, g2]
    o.append(text(250, 262, "the wide row is at the top; pin 1 top left", size=16, fill="#444"))
    o.append(text(750, 262, "the wide row is at the top; pin 1 top right", size=16, fill="#444"))
    o.append(legend_ribbon(40, 305, size=16, dx=104))
    o.append(text(500, 338, "Dot in each cup = ribbon colour used for that pin (pin number = colour code).", size=16, fill="#444"))
    return svg(1000, 352, "".join(o))


def passthrough():
    o = []
    g1, pf = db9(230, 170, 1.9, "female", "Tap female end (solder side)", colours={p: C[p] for p in range(1, 10)})
    g2, pm = db9(770, 170, 1.9, "male", "Tap male end (solder side)", colours={p: C[p] for p in range(1, 10)})
    for p in range(1, 10):
        x1, y1 = pf[p]
        x2, y2 = pm[p]
        mid = 500
        yy = 290 + p * 14
        o.append(wire(f"M {x1} {y1} C {x1} {yy}, {mid - 150} {yy}, {mid} {yy} S {x2} {yy + 0}, {x2} {y2}", C[p], 4.2))
    o += [g1, g2]
    o.append(text(500, 450, "Each pin to the same pin: 1→1, 2→2 … 9→9. About 15 cm of ribbon.", size=18, weight="bold"))
    o.append(text(500, 472, "The pin order differs between the two solder sides; follow the numbers, not the positions.", size=16, fill="#444"))
    return svg(1000, 485, "".join(o))


def leads():
    o = []
    g1, pf = db9(500, 120, 1.7, "female", "Tap female end (solder side): cups 2, 3 and 5 get a second or third wire",
                 colours={p: C[p] for p in range(1, 10)}, highlight=(2, 3, 5))
    g2, px = db9(210, 400, 1.45, "female", None, highlight=(2, 5), empty_mark=(1, 3, 4, 6, 7, 8, 9),
                 colours={2: C[3], 5: C[4]})
    g3, py = db9(790, 400, 1.45, "female", None, highlight=(2, 5), empty_mark=(1, 3, 4, 6, 7, 8, 9),
                 colours={2: C[2], 5: C[1]})
    o.append(wire(f"M {pf[3][0]} {pf[3][1]} C {pf[3][0] - 40} 260, {px[2][0] + 40} 300, {px[2][0]} {px[2][1]}", C[3], 5))
    o.append(wire(f"M {pf[5][0]} {pf[5][1]} C {pf[5][0] - 60} 300, {px[5][0] + 60} 300, {px[5][0]} {px[5][1]}", C[4], 5))
    o.append(wire(f"M {pf[2][0]} {pf[2][1]} C {pf[2][0] + 40} 260, {py[2][0] - 40} 300, {py[2][0]} {py[2][1]}", C[2], 5))
    o.append(wire(f"M {pf[5][0]} {pf[5][1]} C {pf[5][0] + 60} 300, {py[5][0] - 60} 300, {py[5][0]} {py[5][1]}", C[1], 5))
    o += [g1, g2, g3]
    o.append(text(210, 478, "Plug X (female), solder side", size=17, weight="bold"))
    o.append(text(790, 478, "Plug Y (female), solder side", size=17, weight="bold"))
    o.append(text(250, 515, "X: tap 3 → X 2 (orange)   tap 5 → X 5 (yellow)", size=17, weight="bold"))
    o.append(text(750, 515, "Y: tap 2 → Y 2 (red)   tap 5 → Y 5 (brown)", size=17, weight="bold"))
    o.append(text(500, 548, "✕ = leave empty. Pin 3 on X and Y must stay empty: it is the adapter's transmit pin.", size=16.5, fill="#a44d00", weight="bold"))
    return svg(1000, 560, "".join(o))


def cup_steps():
    o = []

    def cup(x, y, filled=False, wires=1, sleeve=False, label=""):
        r = []
        r.append(f'<path d="M {x - 14} {y - 40} L {x - 14} {y + 10} Q {x} {y + 26} {x + 14} {y + 10} L {x + 14} {y - 40}" '
                 f'fill="#e0c46a" stroke="#8a6d1f" stroke-width="2"/>')
        r.append(f'<rect x="{x - 6}" y="{y + 18}" width="12" height="40" fill="#c9a648" stroke="#8a6d1f"/>')
        cols = [RIBBON[3][1], RIBBON[4][1], RIBBON[1][1]][:wires]
        for i, c in enumerate(cols):
            dx = (i - (wires - 1) / 2) * 7
            r.append(wire(f"M {x + dx} {y - 120} L {x + dx} {y - 4}", c, 5))
        if filled:
            r.append(f'<path d="M {x - 14} {y - 36} Q {x} {y - 46} {x + 14} {y - 36} L {x + 14} {y + 8} Q {x} {y + 22} {x - 14} {y + 8} Z" '
                     f'fill="#c8cdd2" stroke="#7d848a" stroke-width="1.5" opacity="0.92"/>')
        if sleeve:
            r.append(f'<rect x="{x - 18}" y="{y - 60}" width="36" height="76" rx="5" fill="#2b2b2b" opacity="0.85"/>')
        r.append(text(x, y + 82, label, size=16))
        return "".join(r)
    o.append(cup(90, 150, label="1 Tin the cup"))
    o.append(f'<path d="M 60 30 L 120 100" stroke="#777" stroke-width="10" stroke-linecap="round"/>')
    o.append(text(60, 24, "iron", size=15))
    o.append(cup(285, 150, wires=1, label="2 Tinned wire in"))
    o.append(cup(480, 150, wires=1, filled=True, label="3 Heat 2–3 s"))
    o.append(cup(675, 150, wires=1, filled=True, sleeve=True, label="4 Sleeve over"))
    o.append(cup(880, 150, wires=3, filled=True, label="Tap cup 5: 3 wires"))
    o.append(text(880, 254, "twist and tin together first", size=15, fill="#444"))
    return svg(1000, 265, "".join(o))


def joints():
    o = []
    # good
    o.append('<path d="M 60 120 L 60 60 Q 90 30 120 60 L 120 120 Z" fill="#c8cdd2" stroke="#7d848a" stroke-width="2"/>')
    o.append(wire("M 90 10 L 90 70", RIBBON[3][1], 6))
    o.append(text(90, 145, "good: smooth, shiny,", size=16))
    o.append(text(90, 160, "fills the cup", size=16))
    # bad balled
    o.append('<path d="M 260 120 L 260 80 L 320 80 L 320 120 Z" fill="#e0c46a" stroke="#8a6d1f"/>')
    o.append('<circle cx="290" cy="62" r="20" fill="#a9aeb3" stroke="#6d7277" stroke-width="2"/>')
    o.append(wire("M 290 10 L 290 50", RIBBON[3][1], 6))
    o.append(text(290, 145, "bad: a ball, dull, does", size=16))
    o.append(text(290, 160, "not wet the cup → reheat", size=16))
    # bad strands
    o.append('<path d="M 460 120 L 460 60 Q 490 40 520 60 L 520 120 Z" fill="#c8cdd2" stroke="#7d848a" stroke-width="2"/>')
    o.append(wire("M 490 10 L 490 70", RIBBON[3][1], 6))
    o.append('<path d="M 492 58 L 540 40 M 494 62 L 545 58" stroke="#b87333" stroke-width="2"/>')
    o.append(text(490, 145, "bad: stray strands", size=16))
    o.append(text(490, 160, "→ cut them off", size=16))
    return svg(600, 170, "".join(o))


def hood_order():
    o = [arrow_marker()]
    o.append(wire("M 40 80 L 600 80", RIBBON[5][1], 9))
    o.append('<rect x="120" y="58" width="90" height="44" rx="6" fill="none" stroke="#333" stroke-width="2" stroke-dasharray="5 3"/>')
    o.append(text(165, 50, "one-piece hood or sleeve", size=16))
    o.append('<rect x="330" y="73" width="40" height="14" fill="#222" opacity="0.85"/>')
    o.append(text(350, 60, "2.4 mm heat-shrink", size=16))
    o.append('<rect x="600" y="40" width="80" height="80" rx="6" fill="#d9dde0" stroke="#6b7177" stroke-width="2"/>')
    o.append(text(640, 140, "connector", size=16))
    o.append(line(260, 120, 140, 120, arrow=True))
    o.append(line(420, 120, 340, 120, arrow=True))
    o.append(text(360, 160, "Slide these on first, away from the end.", size=16.5))
    o.append(text(360, 182, "Hoods made of two halves that screw together go on at the end.", size=16.5))
    return svg(720, 195, "".join(o))


def meter():
    o = []
    o.append('<rect x="30" y="30" width="150" height="210" rx="14" fill="#f2c618" stroke="#7a5c00" stroke-width="2"/>')
    o.append('<rect x="50" y="50" width="110" height="55" rx="4" fill="#cfe3c8" stroke="#555"/>')
    o.append(text(105, 85, "0.0 Ω", size=20, weight="bold"))
    o.append('<circle cx="105" cy="160" r="34" fill="#333"/>')
    o.append('<path d="M 105 160 L 130 140" stroke="#fff" stroke-width="5"/>')
    o.append(text(145, 135, "•))", size=18, anchor="start", weight="bold"))
    o.append(text(105, 225, "continuity (beep)", size=15))
    o.append(wire("M 80 240 C 80 300 260 300 330 220", "#d32f2f", 4))
    o.append(wire("M 130 240 C 130 320 470 320 530 220", "#222222", 4))
    g1, pf = db9(380, 150, 1.0, "female", "tap female end", colours={p: RIBBON[p][1] for p in range(1, 10)})
    g2, pm = db9(640, 150, 1.0, "male", "tap male end", colours={p: RIBBON[p][1] for p in range(1, 10)})
    o += [g1, g2]
    o.append(f'<path d="M 330 220 L {pf[3][0]} {pf[3][1] + 6}" stroke="#d32f2f" stroke-width="2.5"/>')
    o.append(f'<path d="M 530 220 L {pm[3][0]} {pm[3][1] + 6}" stroke="#222" stroke-width="2.5"/>')
    o.append(text(520, 42, "Example: pin 3 to pin 3 must beep.", size=17, weight="bold"))
    return svg(780, 310, "".join(o))


def at_instrument():
    o = [arrow_marker()]
    o.append(box(10, 40, 180, 90, "Instrument PC", "vendor software"))
    o.append(box(810, 40, 170, 90, "Instrument"))
    o.append('<rect x="210" y="55" width="60" height="60" rx="8" fill="#5c6670"/>' + text(240, 92, "F", 22, weight="bold", fill="#fff"))
    o.append('<rect x="400" y="55" width="60" height="60" rx="8" fill="#5c6670"/>' + text(430, 92, "M", 22, weight="bold", fill="#fff"))
    o.append(line(190, 85, 210, 85, width=3))
    for i in range(9):
        o.append(f'<line x1="270" y1="{68 + i * 4}" x2="400" y2="{68 + i * 4}" stroke="{RIBBON[i + 1][1]}" stroke-width="3"/>')
    o.append('<path d="M 460 85 L 810 85" stroke="#444" stroke-width="7"/>')
    o.append(text(635, 74, "instrument cable (unchanged)", size=16))
    o.append(text(150, 24, "tap female end: onto the PC's COM port", size=15, anchor="start"))
    o.append(text(400, 146, "tap male end: instrument cable plugs in here", size=15, anchor="start"))
    o.append(wire("M 230 115 C 230 170 180 190 160 220", RIBBON[3][1], 4))
    o.append(wire("M 250 115 C 250 170 300 190 330 220", RIBBON[2][1], 4))
    o.append(box(90, 220, 140, 50, "X → adapter", size=16))
    o.append(box(260, 220, 140, 50, "Y → adapter", size=16))
    o.append(line(160, 270, 220, 310, width=3))
    o.append(line(330, 270, 280, 310, width=3))
    o.append(box(130, 310, 240, 58, "Laptop on battery", "LibreTap", size=17))
    o.append(text(560, 200, "If a plug does not fit (wrong gender), use a", size=16.5, anchor="start"))
    o.append(text(560, 220, "DB-9 gender changer (male–male or", size=16.5, anchor="start"))
    o.append(text(560, 240, "female–female). The tap still works.", size=16.5, anchor="start"))
    return svg(1000, 380, "".join(o))


def build():
    b = []
    b.append(f"""<section class="first">
<h1>Serial tap: build guide</h1>
<p class="sub">A passive DB-9 tap for LibreTap: the laptop listens to both directions of an RS-232 link and cannot send anything.</p>
<figure>{overview()}<figcaption>Figure 1. The finished tap in use. The PC and the instrument keep talking through the pass-through; leads X and Y copy each direction to one USB adapter each.</figcaption></figure>
<h3>How it works</h3>
<p>RS-232 uses one wire per direction: DB-9 pin 3 carries what the PC sends, pin 2 what it receives; pin 5 is ground.
The tap is a short extension cable (all nine pins straight through) with two extra leads. Lead X connects pin 3 to the
receive pin (2) of adapter X; lead Y connects pin 2 to the receive pin of adapter Y. Nothing is connected to the
adapters' transmit pin (3), so the laptop cannot put anything on the line.</p>
<p class="note">Time: about 2 hours the first time. Not yet tested on a real instrument.</p>
</section>""")
    b.append(f"""<section>
<h2>Parts and tools</h2>
<table>
<tr><th>Part (per tap)</th><th>Qty</th><th>Note</th></tr>
<tr><td>DB-9 male connector, solder cups</td><td>1</td><td>tap male end</td></tr>
<tr><td>DB-9 female connector, solder cups</td><td>3</td><td>tap female end, plug X, plug Y</td></tr>
<tr><td>DB-9 hoods (backshells)</td><td>4</td><td>one per connector</td></tr>
<tr><td>10-core rainbow ribbon cable</td><td>about 1.2 m</td><td>15 cm pass-through + 2 × 50 cm leads</td></tr>
<tr><td>Heat-shrink tubing, 2.4 mm</td><td>about 30 cm</td><td>cut into 8 mm pieces, one per cup</td></tr>
<tr><td>USB–RS-232 adapter (male DB-9)</td><td>2</td><td>not part of the tap; needed to use it</td></tr>
</table>
<p><b>Tools:</b> soldering iron with stand, solder (0.5–1 mm, rosin core), wire stripper or sharp knife, side cutters,
a helping hand or a vice to hold the connector, multimeter with continuity (beep) mode, lighter or hot-air gun for the
heat-shrink (or the side of the iron), masking tape and a pen, cable ties.</p>
<h3>Ribbon colours</h3>
<p>Rainbow ribbon follows the resistor colour code. Use core <i>n</i> for pin <i>n</i>: then the colour tells the pin.
Check the order on your ribbon before starting; tear off the black (10th) core.</p>
<figure>{svg(1000, 40, legend_ribbon(40, 26, size=16, dx=104))}</figure>
<p>Separate cores by nicking the ribbon between two cores with a knife and pulling them apart by hand.</p>
</section>""")
    b.append(f"""<section>
<h2>Soldering in brief</h2>
<ul>
<li><b>Safety.</b> Work in a ventilated room or with fume extraction; do not breathe the smoke. Wear eye protection
(clipped wire ends fly). The iron goes back into its stand every time you put it down; the tip is above 300 °C. Leaded
solder: wash your hands afterwards and do not eat at the bench.</li>
<li><b>Solder.</b> 60/40 tin–lead melts lower and is easier for beginners; lead-free is required in some workshops.
Either works.</li>
<li><b>Temperature.</b> 330–350 °C for leaded solder, 350–380 °C for lead-free.</li>
<li><b>Tin the tip.</b> Wipe the hot tip on the damp sponge or brass wool, then melt a little solder onto it. A shiny tip
transfers heat; a dull, black one does not.</li>
<li><b>Tin the wire.</b> Strip 3 mm, twist the strands, heat them and feed in a little solder until the strands are
coated. Tinned ends go into cups without loose strands.</li>
<li><b>Heat the joint, not the solder.</b> Hold the tip against cup and wire, then feed solder into the joint (not
onto the tip). 2–3 seconds is enough. Remove the solder, then the iron.</li>
<li><b>Don't move it</b> until the solder has set (a second or two). A joint moved while setting looks grainy and is weak.</li>
<li><b>Heat-shrink goes on before soldering.</b> Slide a piece onto the wire, keep it away from the heat while
soldering, then slide it over the cup and shrink it.</li>
</ul>
<figure>{joints()}<figcaption>Figure 2. Judging a joint.</figcaption></figure>
<div class="check"><b>Practice first.</b> Solder two scrap wires together and one wire into a spare cup before starting.
Pull on them: a good joint holds.</div>
</section>""")
    b.append(f"""<section>
<h2>1. Find the pins</h2>
<p>You solder on the back of each connector (the solder side). Seen from there, the pin numbers are mirrored compared
with the front, and male and female connectors are mirrored to each other. Most connectors have the numbers moulded
into the plastic next to the cups: <b>trust those numbers</b> over any drawing.</p>
<figure>{pin_views()}<figcaption>Figure 3. Solder side of both connector types, with the ribbon colour for each pin.</figcaption></figure>
<ol class="steps">
<li>Hold each connector with the solder side towards you and the wider row of cups at the top.</li>
<li>Find pin 1 on each and compare with Figure 3. Mark pin 1 with a pen dot on the plastic.</li>
</ol>
<div class="check"><b>Check.</b> Female, solder side: pin 1 top left. Male, solder side: pin 1 top right.</div>
<h2 style="margin-top:6mm">2. Prepare the ribbon</h2>
<ol class="steps">
<li>Cut 15 cm of ribbon and tear off the black core: 9 cores remain (brown … white). This is the pass-through.</li>
<li>Cut two 50 cm pieces for the leads and tear off two 2-core strips: <b>orange + yellow</b> (lead X) and
<b>red + brown</b> (lead Y). Label them X and Y with tape at both ends.</li>
<li>Split each end of all cores 3 cm apart, strip 3 mm and tin each end.</li>
</ol>
<figure>{hood_order()}<figcaption>Figure 4. Before soldering any cable: hood parts that must slide over it, and heat-shrink.</figcaption></figure>
</section>""")
    b.append(f"""<section>
<h2>3. Solder the pass-through</h2>
<figure>{passthrough()}<figcaption>Figure 5. The pass-through, both connectors seen from the solder side.</figcaption></figure>
<ol class="steps">
<li>Clamp the <b>female</b> connector solder side up. Tin all nine cups (Figure 6, step 1).</li>
<li>Slide an 8 mm piece of heat-shrink onto each of the nine cores.</li>
<li>Solder brown into cup 1, red into cup 2 … white into cup 9. Start with the bottom row so the iron does not have to
reach past finished joints.</li>
<li>Do the same on the <b>male</b> connector at the other end of the ribbon: brown into 1 … white into 9. Do not twist the
ribbon so far that cores cross.</li>
<li>Do <b>not</b> shrink the sleeves yet on cups 2, 3 and 5 of the female end: they get more wires in step 4. Shrink the others.</li>
</ol>
<figure>{cup_steps()}<figcaption>Figure 6. Soldering one cup. Cup 5 of the tap's female end holds three wires.</figcaption></figure>
<div class="check"><b>Check.</b> Each colour sits in the cup with its number on both connectors (Figure 3). No solder
bridges between neighbouring cups. Sleeves shrunk on 1, 4, 6, 7, 8, 9 (female) and on all nine (male).</div>
</section>""")
    b.append(f"""<section>
<h2>4. Solder the leads X and Y</h2>
<figure>{leads()}<figcaption>Figure 7. The leads, all connectors seen from the solder side. Plugs X and Y are female.</figcaption></figure>
<ol class="steps">
<li>On the tap's female end, reheat cup 3 and add the <b>orange</b> core of lead X.</li>
<li>Reheat cup 2 and add the <b>red</b> core of lead Y.</li>
<li>Twist the <b>yellow</b> (X) and <b>brown</b> (Y) cores together, tin them, and add them to cup 5. Cup 5 now holds
three wires. If the heat-shrink no longer fits over cup 5, leave it off there and make sure no strand touches cup 4, 6 or 9.</li>
<li>At the other end of lead X: orange into cup 2 of plug X, yellow into cup 5.</li>
<li>At the other end of lead Y: red into cup 2 of plug Y, brown into cup 5.</li>
<li>Leave all other cups of X and Y empty. <b>Pin 3 on X and Y must stay empty</b>: it is the adapter's transmit pin.</li>
<li>Shrink the remaining sleeves.</li>
</ol>
<div class="check"><b>Check.</b> Lead X: orange from tap 3 to X 2, yellow from tap 5 to X 5. Lead Y: red from tap 2 to
Y 2, brown from tap 5 to Y 5. X and Y have only cups 2 and 5 used.</div>
</section>""")
    b.append(f"""<section>
<h2>5. Test with the multimeter</h2>
<p>Set the multimeter to continuity (the beep symbol •))). Touch the two probes together: it beeps. On the male end the
pins stick out; on female ends, push a thin wire or a paper clip into the socket and touch that.</p>
<figure>{meter()}<figcaption>Figure 8. Testing one pin of the pass-through.</figcaption></figure>
<table>
<tr><th>Probe 1</th><th>Probe 2</th><th>Expected</th></tr>
<tr><td>tap female pin 1, 2 … 9</td><td>tap male, same pin</td><td>beep (all nine)</td></tr>
<tr><td>any pin</td><td>its neighbours, same connector</td><td>no beep (no bridges)</td></tr>
<tr><td>plug X pin 2</td><td>tap pin 3</td><td>beep</td></tr>
<tr><td>plug Y pin 2</td><td>tap pin 2</td><td>beep</td></tr>
<tr><td>plug X pin 5, plug Y pin 5</td><td>tap pin 5</td><td>beep</td></tr>
<tr><td>plug X pin 3, plug Y pin 3</td><td>any tap pin</td><td>no beep</td></tr>
<tr><td>plug X pin 2</td><td>plug Y pin 2</td><td>no beep</td></tr>
</table>
<div class="check"><b>Check.</b> All rows as expected. Fix any failure now; a wrong pin is much harder to find at the instrument.</div>
</section>""")
    b.append(f"""<section>
<h2>6. Close the hoods</h2>
<ol class="steps">
<li>Lay the cable into the hood's strain relief and screw the two halves together (or slide a one-piece hood forward).</li>
<li>The tap's female end has three cables in one hood (ribbon, X, Y). If they do not fit, close the hood around the
ribbon and fix X and Y to it with a cable tie just behind the hood, so no pull reaches the cups.</li>
<li>Fit the thumb screws (or the hex standoffs supplied with the hood).</li>
</ol>
<div class="check"><b>Check.</b> Repeat the first and the last three rows of the table: closing a hood can pull a wire loose.</div>
<h2 style="margin-top:6mm">7. Use it at the instrument</h2>
<figure>{at_instrument()}<figcaption>Figure 9. The tap inserted between the PC and the instrument cable.</figcaption></figure>
<ol class="steps">
<li>Close the vendor software (OPUS, …).</li>
<li>Unplug the instrument cable from the PC's COM port. Plug the tap's female end onto the COM port and the instrument
cable into the tap's male end.</li>
<li>Plug X and Y onto the two USB–RS-232 adapters, and the adapters into the laptop. Run the laptop on battery
(avoids a ground loop between the laptop's and the PC's mains supplies).</li>
<li>Start LibreTap. Connections shows <b>Serial tap ready</b> and names the two adapters. If the PC and instrument sides
are the wrong way round, press <b>Swap sides</b>.</li>
<li>Start the vendor software and check that it talks to the instrument as before.</li>
<li>Unsure of the speed: press <b>Find speed</b> while the software talks to the instrument.</li>
</ol>
</section>""")
    b.append(f"""<section>
<h2>Troubleshooting</h2>
<table>
<tr><th>Symptom</th><th>Likely cause</th><th>Action</th></tr>
<tr><td>Vendor software no longer reaches the instrument</td><td>pass-through: missing pin, bridge, or a handshake pin not through</td><td>Repeat test 5, rows 1–2. All nine pins must pass.</td></tr>
<tr><td>Plug does not fit the COM port or the cable</td><td>connector gender</td><td>DB-9 gender changer (male–male or female–female).</td></tr>
<tr><td>No data on one side</td><td>lead wiring, or the PC's port is not the one in use</td><td>Repeat test 5, rows 3–5. Check which COM port the software uses.</td></tr>
<tr><td>Data, but unreadable</td><td>wrong speed or format</td><td>LibreTap: Find speed. Common: 9600, 19200, 4800; 8N1 or 7E1.</td></tr>
<tr><td>Both sides show the same data</td><td>X and Y both on the same signal</td><td>Check: X pin 2 ↔ tap 3, Y pin 2 ↔ tap 2.</td></tr>
<tr><td>Adapter not listed in LibreTap (Linux)</td><td>no permission for serial ports</td><td><code>sudo usermod -aG dialout $USER</code>, then log out and in.</td></tr>
<tr><td>Wire came loose at a cup</td><td>no strain relief</td><td>Resolder; cable tie behind the hood.</td></tr>
</table>
<p class="footer">LibreTap — github.com/ohlincha/libretap · docs/hardware/serial-tap-build · MIT licence</p>
</section>""")
    return page("Serial tap: build guide", "", "\n".join(b))
