#!/bin/sh
# Tick's "Start a program" under Wine (Xvfb): the program list (Start menu shortcut made with the
# system's IShellLink, installed-software entry, uninstaller left out), starting from the shortcut
# (its arguments and Start-in folder), restarting a running program, Pick window + listening
# without restarting, and a shortcut given on the command line (= dropped onto Tick.exe).
#   tests/wine-test-programs.sh WINVER SCRATCH        WINVER: winxp | win7 | win10
# Needs: make all tools-test; a Wine (WINE=..., default ~/opt/wine-11.20-amd64-wow64/bin/wine),
# xvfb-run, xdotool, python3.  Makes its own Wine prefix in SCRATCH (wineboot) and says so if Wine fails.
set -e
here=$(cd "$(dirname "$0")/.." && pwd)
ver=${1:-win7}
S=${2:-/tmp/tick-wine-programs}
WINE=${WINE:-$HOME/opt/wine-11.20-amd64-wow64/bin/wine}
WINESERVER=${WINESERVER:-$(dirname "$WINE")/wineserver}
export WINEPREFIX="$S/pfx" WINEDEBUG=-all WINEDLLOVERRIDES="mscoree,mshtml="
mkdir -p "$S"
if [ ! -f "$WINEPREFIX/drive_c/windows/syswow64/kernel32.dll" ]; then
    (unset DISPLAY; timeout 300 "$WINE" wineboot -i >/dev/null 2>&1; "$WINESERVER" -w) || true
fi
"$WINE" cmd /c ver >/dev/null 2>&1 || { echo "WINE CANNOT RUN (prefix $WINEPREFIX): test not run"; exit 3; }
"$WINE" reg add 'HKCU\Software\Wine' /v Version /d "$ver" /f >/dev/null 2>&1
"$WINE" reg add 'HKLM\Software\Wine\Ports' /v COM1 /d "$S/com1-link" /f >/dev/null 2>&1
"$WINESERVER" -k 2>/dev/null || true
t="$WINEPREFIX/drive_c/tickp"
rm -rf "$t"; mkdir -p "$t/work"
for f in Tick.exe tickctl.exe fakewinvendor.exe mklink.exe lnkdump.exe; do cp "$here/build/$f" "$t/"; done
out="$S/$ver"; rm -rf "$out"; mkdir -p "$out"
python3 -I "$here/tests/fake_instrument.py" "$S/com1-link" 400 > "$out/instrument.log" 2>&1 &
inst=$!

cat > "$S/run.sh" <<EOF
cd "$t"
W="$WINE"
ctl() { "\$W" tickctl.exe "\$@" 2>/dev/null | tr -d '\r'; }
progs=\$("\$W" cmd /c echo %APPDATA% 2>/dev/null | tr -d '\r')\\\\Microsoft\\\\Windows\\\\Start\ Menu\\\\Programs
echo "programs folder: \$progs"
"\$W" cmd /c mkdir "\$progs\\\\Fake Vendor" >/dev/null 2>&1
"\$W" mklink.exe "\$progs\\\\Fake Vendor\\\\Fake VendorSoft.lnk" 'C:\\tickp\\fakewinvendor.exe' COM1 'C:\\tickp\\work' 'Acquisition' | tr -d '\r'
"\$W" mklink.exe "\$progs\\\\Fake Vendor\\\\Uninstall Fake Vendor.lnk" 'C:\\tickp\\fakewinvendor.exe' /uninstall | tr -d '\r'
echo "== Tick's reading of the shortcut Windows wrote:"
"\$W" lnkdump.exe "\$progs\\\\Fake Vendor\\\\Fake VendorSoft.lnk" | tr -d '\r'
"\$W" reg add 'HKLM\\Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\FakeVendorSoft' /v DisplayName /d 'Fake VendorSoft Suite' /f >/dev/null 2>&1
"\$W" reg add 'HKLM\\Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\FakeVendorSoft' /v DisplayIcon /d 'C:\\tickp\\fakewinvendor.exe,0' /f >/dev/null 2>&1
"\$W" reg add 'HKLM\\Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\OldAnalyzer' /v DisplayName /d 'Old Analyzer 1.0' /f >/dev/null 2>&1
"\$W" reg add 'HKLM\\Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\OldAnalyzer' /v InstallLocation /d 'C:\\tickp\\work' /f >/dev/null 2>&1

echo "== 1: list, start from the shortcut"
"\$W" Tick.exe &
sleep 6
ctl programs "Fake VendorSoft"
sleep 8
ctl status
ctl close; sleep 1; ctl answer yes || true
sleep 3
echo "vendor still running after Tick closed: \$("\$W" cmd /c tasklist 2>/dev/null | grep -ci fakewinvendor || true)"

echo "== 2: the program is running: restart it under Tick"
"\$W" Tick.exe &
sleep 6
ctl programs "Running: Fake VendorSoft"
ctl answer yes
sleep 10
ctl status
ctl close; sleep 1; ctl answer yes || true
sleep 3

echo "== 3: Pick window, listen without restarting"
"\$W" Tick.exe &
sleep 6
ctl programs --pick
sleep 1
wid=\$(xdotool search --name "Fake VendorSoft" | head -1)
eval \$(xdotool getwindowgeometry --shell "\$wid")
xdotool mousemove \$((X + WIDTH / 2)) \$((Y + HEIGHT / 2)) click 1
echo "clicked the Fake VendorSoft window"
ctl answer no
sleep 8
ctl status
ctl close; sleep 1; ctl answer yes || true
sleep 2
"\$W" cmd /c taskkill /im fakewinvendor.exe >/dev/null 2>&1 || true
sleep 3

echo "== 4: a shortcut given to Tick.exe (as when dropped onto its icon)"
"\$W" Tick.exe "\$progs\\\\Fake Vendor\\\\Fake VendorSoft.lnk" &
sleep 10
ctl status
ctl close; sleep 1; ctl answer yes || true
sleep 2
"\$W" cmd /c taskkill /im fakewinvendor.exe >/dev/null 2>&1 || true
sleep 2
"$WINESERVER" -k
EOF
timeout 400 xvfb-run -a -s "-screen 0 1024x768x24" sh "$S/run.sh" > "$out/run.log" 2>&1 || true
kill $inst 2>/dev/null || true
cat "$out/run.log"
echo "== vendor starts (command line, working folder):"
cat "$t/vendor-start.log" 2>/dev/null | tr -d '\r'
for d in "$t"/TickSessions/*/; do
    echo "-- session $(basename "$d")"
    python3 -I "$here/tests/check_session.py" "$d" >/dev/null && echo "   session ok" || echo "   SESSION CHECK FAILED"
    echo "   data events: $(grep -c '"kind":"data"' "$d/events.jsonl" || true)"
    grep -E 'Started|Listening to running|stopped listening' "$d/events.jsonl" | cut -c1-180 || true
done
