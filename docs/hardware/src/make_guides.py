"""Writes docs/hardware/*.html from the guide sources (drawings share one geometry). Run: python3 src/make_guides.py"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import serial_tap  # noqa: E402

GUIDES = {"serial-tap-build.html": serial_tap.build}
try:
    import usb_sniffer  # noqa: E402
    GUIDES["usb-sniffer-build.html"] = usb_sniffer.build
except ImportError:
    pass
try:
    import flashing  # noqa: E402
    GUIDES["pico-firmware-flashing.html"] = flashing.build
except ImportError:
    pass

try:
    import switch_setup  # noqa: E402
    GUIDES["switch-setup.html"] = switch_setup.build
except ImportError:
    pass

out_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
for name, fn in GUIDES.items():
    with open(os.path.join(out_dir, name), "w", encoding="utf-8") as f:
        f.write(fn())
    print("wrote", name)
