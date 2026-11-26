#!/bin/sh
# End-to-end tests of Tick for Linux under Xvfb (run from tick/: tests/linux-test.sh SCRATCH).
#  A  terminal UI in an xterm (keys s, m + note + Enter, y, s, q) while xclock ticks;
#     Tick's own xterm must be grey in the pictures (WINDOWID mask)
#  B  tick run + control socket (mark, sync, status) with a fake vendor program
#     talking to a fake instrument on a pseudo-terminal
#  C  worst cases: kill -9 tick while listening; kill -9 the program; session
#     folder removed while recording; quit tick while the program runs
# Needs: build/linux/tick, build/linux/fakevendor_linux, xvfb-run, xterm, xclock, xdotool,
# import (ImageMagick), python3.
set -e
here=$(cd "$(dirname "$0")/.." && pwd)
S=${1:-/tmp/tick-linux-test}
rm -rf "$S"; mkdir -p "$S"
T="$here/build/linux/tick" V="$here/build/linux/fakevendor_linux"
export T V S here

cat > "$S/a.sh" <<'EOS'
xclock -update 1 -geometry 200x200+20+20 &
sleep 1
xterm -geometry 90x16+250+300 -e "$T" --dir "$S/A" --control "$S/a.sock" &
sleep 3
w=$(xdotool search --class xterm | head -1)
xdotool windowfocus "$w" 2>/dev/null || true
xdotool type --window "$w" s; sleep 3
xdotool type --window "$w" m; xdotool type --window "$w" "first note"; xdotool key --window "$w" Return
sleep 1
xdotool type --window "$w" y; sleep 0.3
import -window root "$S/A-sync.png"
sleep 2
import -window root "$S/A-display.png"
xdotool type --window "$w" s; sleep 1
xdotool type --window "$w" q; sleep 2
kill %1 2>/dev/null || true
EOS
echo "== A: terminal UI in xterm"
timeout 60 xvfb-run -a -s "-screen 0 800x600x24" sh "$S/a.sh" >/dev/null 2>&1 || true
for d in "$S"/A/*/; do python3 -I "$here/tests/check_session.py" "$d" "$S/A-rebuilt.png"; grep -E '"kind":"(mark|sync)"' "$d/events.jsonl"; done

python3 -I "$here/tests/fake_instrument.py" "$S/port" 300 > "$S/instrument.log" 2>&1 &
inst=$!
sleep 1
export FAKEVENDOR_LOG="$S/vendor.log"

echo "== B: tick run + control socket"
timeout 60 xvfb-run -a -s "-screen 0 640x480x24" "$T" run --no-tui --until-exit --dir "$S/B" --control "$S/b.sock" -- "$V" "$S/port" 8 300 > "$S/B.log" 2>&1 &
sleep 1.5
"$T" ctl --control "$S/b.sock" mark "via ctl"
"$T" ctl --control "$S/b.sock" sync >/dev/null
"$T" ctl --control "$S/b.sock" status
wait $! || true
cat "$S/vendor.log"; rm -f "$S/vendor.log"
for d in "$S"/B/*/; do python3 -I "$here/tests/check_session.py" "$d"; grep -E '"kind":"(mark|sync|status|config)"' "$d/events.jsonl" | cut -c1-160; done

echo "== C1: kill -9 tick while it listens"
xvfb-run -a -s "-screen 0 640x480x24" "$T" run --no-tui --dir "$S/C1" --control "$S/c1.sock" -- "$V" "$S/port" 20 300 > "$S/C1.log" 2>&1 &
sleep 2
pkill -9 -x -f "$T run --no-tui --dir $S/C1 --control $S/c1.sock -- $V $S/port 20 300" || true
sleep 10
echo "program after tick was killed: $(cat "$S/vendor.log")"; rm -f "$S/vendor.log"
for d in "$S"/C1/*/; do python3 -I "$here/tests/check_session.py" "$d"; done
ls /dev/shm | grep -c tick-ring || true

echo "== C2: kill -9 the program"
xvfb-run -a -s "-screen 0 640x480x24" "$T" run --no-tui --until-exit --dir "$S/C2" --control "$S/c2.sock" -- "$V" "$S/port" 40 300 > "$S/C2.log" 2>&1 &
sleep 2
pkill -9 -x -f "$V $S/port 40 300" || true
sleep 2
for d in "$S"/C2/*/; do python3 -I "$here/tests/check_session.py" "$d"; grep -E 'ended' "$d/events.jsonl"; done

echo "== C3: session folder removed while recording and listening"
xvfb-run -a -s "-screen 0 640x480x24" "$T" run --no-tui --until-exit --dir "$S/C3" --control "$S/c3.sock" -- "$V" "$S/port" 20 300 > "$S/C3.log" 2>&1 &
sleep 2
rm -rf "$S/C3"
sleep 1
"$T" ctl --control "$S/c3.sock" status || true
sleep 8
echo "program: $(cat "$S/vendor.log")"; rm -f "$S/vendor.log"
grep -i "error\|could not" "$S/C3.log" | head -3 || true

echo "== C4: quit tick while the program runs"
xvfb-run -a -s "-screen 0 640x480x24" "$T" run --no-tui --dir "$S/C4" --control "$S/c4.sock" -- "$V" "$S/port" 20 300 > "$S/C4.log" 2>&1 &
sleep 2
"$T" ctl --control "$S/c4.sock" quit >/dev/null || true
sleep 10
echo "program after tick quit: $(cat "$S/vendor.log")"
for d in "$S"/C4/*/; do python3 -I "$here/tests/check_session.py" "$d"; grep -E 'stopped listening' "$d/events.jsonl" | cut -c1-140; done
kill $inst 2>/dev/null || true
