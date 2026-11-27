#!/bin/sh
# Tick for Linux: choosing the program (run from tick/: tests/linux-test-programs.sh SCRATCH).
#  E  tick run "Fake Vendor": an applications-menu name -> its Exec and Path (.desktop)
#  F  the window (--gui): Start a program -> list (keyboard) -> start; recording and listening
#  G  restart: the program already runs; Tick asks, Yes (clicked) closes it and starts it again
#     (only the test's own program file can match: shells and interpreters never do)
#  H  Pick window: click an xterm running the vendor program -> Yes -> restarted under Tick
#  I  terminal: p (list), Esc, o (settings) 1 + Esc -> tick.ini written
#  K  drop onto the window: a text file (refused), a menu entry with spaces in its path (started)
#  L  isolation: a copy of the program outside this test's process tree is never offered for restart
# Isolation: TICK_TEST_ROOT_PID limits Tick to this script's own processes; windows in own Xvfb.
# Needs: make linux linux-tools-test; xvfb-run, xterm, xdotool, import, python3.
set -e
here=$(cd "$(dirname "$0")/.." && pwd)
# Isolation: Tick lists, matches and closes only processes started under this script (never the
# user's own programs), and every window test runs in its own Xvfb (checked in f.sh).
export TICK_TEST_ROOT_PID=$$
S=${1:-/tmp/tick-linux-programs}
[ -d "$S/bin" ] && chmod u+w "$S/bin"; rm -rf "$S"; mkdir -p "$S/apps/applications" "$S/work" "$S/cfg"
mkdir -p "$S/bin"; cp "$here/build/linux/tick" "$S/bin/"; chmod a-w "$S/bin"   # settings then go to XDG_CONFIG_HOME
T="$S/bin/tick" V="$here/build/linux/fakevendor_linux"
# a menu with one application: the vendor program, started in its own folder
cat > "$S/apps/applications/fakevendor.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Fake Vendor
Exec=sh -c "pwd > cwd.txt; exec $V $S/port 25 300" %U
Path=$S/work
EOF
cat > "$S/apps/applications/fakevendor-direct.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Fake Vendor Direct
Exec=$V $S/port 25 300
Path=$S/work
EOF
export XDG_DATA_HOME="$S/apps" XDG_DATA_DIRS="$S/none" XDG_CONFIG_HOME="$S/cfg" T V S here
python3 -I "$here/tests/fake_instrument.py" "$S/port" 400 > "$S/instrument.log" 2>&1 &
inst=$!
sleep 1

echo "== E: tick run with a menu name"
"$T" run --no-tui --no-screen --until-exit --dir "$S/E" --control "$S/e.sock" "Fake Vendor" > "$S/E.log" 2>&1
echo "working folder: $(cat "$S/work/cwd.txt")"
for d in "$S"/E/*/; do python3 -I "$here/tests/check_session.py" "$d" >/dev/null && echo "session ok"; echo "data events: $(grep -c '"kind":"data"' "$d/events.jsonl")"; done

cat > "$S/f.sh" <<'EOS'
[ -n "$TICK_TEST_ROOT_PID" ] || { echo "refusing to run: TICK_TEST_ROOT_PID is not set" >&2; exit 1; }
case "$XAUTHORITY" in */xvfb-run.*) ;; *) echo "refusing to run: not inside its own Xvfb (xvfb-run)" >&2; exit 1 ;; esac
win() { xdotool search --name "Tick 0.5" | head -1; }
geom() { eval $(xdotool getwindowgeometry --shell "$(win)"); }
click() { geom; xdotool mousemove $((X + $1)) $((Y + $2)) click 1; }
row_of() { "$T" ctl --control "$1" programs | python3 -c "import json,sys; l=[p['label'] for p in json.load(sys.stdin)['programs']]; print(next(i for i,x in enumerate(l) if x.startswith(sys.argv[1])))" "$2"; }

echo "== F: the window: Start a program, choose from the list"
"$T" --gui --dir "$S/F" --control "$S/f.sock" > "$S/F.log" 2>&1 &
sleep 2
import -window root "$S/F-start.png"
click 90 330                      # Start a program...
sleep 1.5
n=$(row_of "$S/f.sock" "Fake Vendor")
echo "Fake Vendor is row $n"
w=$(win)
i=0; while [ $i -lt "$n" ]; do xdotool key --window "$w" Down; i=$((i + 1)); done
import -window root "$S/F-list.png"
xdotool key --window "$w" Return
sleep 4
import -window root "$S/F-listening.png"
"$T" ctl --control "$S/f.sock" status | cut -c1-400
sleep 6
"$T" ctl --control "$S/f.sock" quit > /dev/null
sleep 1

echo "== G: the program is running already: restart it"
(cd "$S/work" && exec "$V" "$S/port" 40 300) > /dev/null 2>&1 &
sleep 1
"$T" --gui --dir "$S/G" --control "$S/g.sock" > "$S/G.log" 2>&1 &
sleep 2
"$T" ctl --control "$S/g.sock" open "Fake Vendor Direct" | python3 -c "import json,sys; print('question:', json.load(sys.stdin)['question'])"
sleep 0.5
import -window root "$S/G-question.png"
click 65 276                      # Yes
sleep 5
"$T" ctl --control "$S/g.sock" status | cut -c1-300
sleep 4
"$T" ctl --control "$S/g.sock" quit > /dev/null
sleep 1

echo "== H: Pick window (an xterm running the vendor program)"
xterm -T "Vendor terminal" -geometry 60x10+500+20 -e "$V" "$S/port" 200 300 &
vt=$!
sleep 2
"$T" --gui --dir "$S/H" --control "$S/h.sock" > "$S/H.log" 2>&1 &
sleep 2
click 90 330
sleep 1.5
click 252 330                     # Pick window
sleep 1
xt=$(xdotool search --name "Vendor terminal" | head -1)
eval $(xdotool getwindowgeometry --shell "$xt")
xdotool mousemove $((X + 50)) $((Y + 50)) click 1
sleep 1
"$T" ctl --control "$S/h.sock" status | python3 -c "import json,sys; print('question:', json.load(sys.stdin)['question'])"
import -window root "$S/H-question.png"
click 65 276                      # Yes
sleep 6
"$T" ctl --control "$S/h.sock" status | cut -c1-300
import -window root "$S/H-listening.png"
"$T" ctl --control "$S/h.sock" quit > /dev/null
sleep 1
kill $vt 2>/dev/null || true       # only the xterm this test started

echo "== I: terminal: p, Esc, o, 1, Esc"
xterm -geometry 100x30+0+0 -e "$T" --dir "$S/I" --control "$S/i.sock" &
sleep 2
w=$(xdotool search --class xterm | tail -1)
xdotool windowfocus "$w" 2>/dev/null || true; sleep 0.3      # xterm ignores sent keys: type into the focus
xdotool type p; sleep 1.5
import -window root "$S/I-list.png"
xdotool key Down Down; sleep 0.3
import -window root "$S/I-list2.png"
xdotool key Escape; sleep 0.3
xdotool type o; sleep 0.5
xdotool type 1; sleep 0.3
import -window root "$S/I-settings.png"
xdotool key Escape; sleep 0.5
xdotool type q; sleep 1

echo "== K: drop files onto the window (XDND, as a file manager does)"
mkdir -p "$S/my apps"; cp "$S/apps/applications/fakevendor-direct.desktop" "$S/my apps/fake vendor.desktop"
echo notes > "$S/notes.txt"
"$T" --gui --dir "$S/K" --control "$S/k.sock" > "$S/K.log" 2>&1 &
sleep 2
D="$here/build/linux/xdnd_drop"
"$D" "$(win)" "file://$S/notes.txt
"
"$T" ctl --control "$S/k.sock" status | python3 -c "import json,sys; print('message:', json.load(sys.stdin)['message'])"
"$D" "$(win)" "$(printf '# dropped\r\nfile://%s\r\n' "$(echo "$S/my apps/fake vendor.desktop" | sed 's/ /%20/g')")"
sleep 4
"$T" ctl --control "$S/k.sock" status | cut -c1-300
import -window root "$S/K-listening.png"
sleep 4
"$T" ctl --control "$S/k.sock" quit > /dev/null
sleep 1

echo "== L: isolation: the same program running outside this test is not offered for restart"
( ( cd "$S/work" && exec "$V" "$S/port" 40 300 > /dev/null 2>&1 ) & )   # double fork: reparented, not under TICK_TEST_ROOT_PID
sleep 0.5
outside=$(pgrep -n -f "^$V $S/port 40 300\$" || true)
"$T" --gui --dir "$S/L" --control "$S/l.sock" > "$S/L.log" 2>&1 &
sleep 2
"$T" ctl --control "$S/l.sock" open "Fake Vendor Direct" | python3 -c "import json,sys; q=json.load(sys.stdin)['question']; print('question:', q or '(none: started anew)')"
sleep 2
[ -n "$outside" ] && kill -0 "$outside" 2>/dev/null && echo "outside copy still running (untouched)"
"$T" ctl --control "$S/l.sock" quit > /dev/null
sleep 1
[ -n "$outside" ] && kill "$outside" 2>/dev/null || true    # started by this test
EOS
timeout 150 xvfb-run -a -s "-screen 0 1024x768x24" sh "$S/f.sh" 2>&1 | grep -v XGetInputFocus || true
kill $inst 2>/dev/null || true
for x in F G H K; do
    for d in "$S"/$x/*/; do
        [ -d "$d" ] || continue
        python3 -I "$here/tests/check_session.py" "$d" >/dev/null && echo "$x: session ok" || echo "$x: SESSION CHECK FAILED"
        echo "$x: data events: $(grep -c '"kind":"data"' "$d/events.jsonl" || true)"
        grep -E '"Started' "$d/events.jsonl" | cut -c1-200 || true
    done
done
echo "settings file:"; cat "$S/cfg/tick/tick.ini" 2>/dev/null || echo "(none)"
echo "screenshots: $S"
