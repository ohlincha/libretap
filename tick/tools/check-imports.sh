#!/bin/sh
# Fail if a PE file imports anything that is not in tools/win95-imports.txt
# (functions present in both Windows 95 and Windows 2000), or if its OS/subsystem
# version is above 4.0 (Windows 95/2000 refuse to start it).
# Usage: tools/check-imports.sh build/Tick.exe [build/TickHook.dll ...]
# OBJDUMP may name llvm-objdump or i686-w64-mingw32-objdump.
set -e
here=$(dirname "$0")
OBJDUMP=${OBJDUMP:-llvm-objdump}
status=0
for f in "$@"; do
    dump=$("$OBJDUMP" -p "$f")
    imports=$(printf '%s\n' "$dump" | awk '/DLL Name:/ {d = tolower($3)} /^ +[0-9]+ +[A-Za-z_?@]/ && d {print d, $2}')
    bad=$(printf '%s\n' "$imports" | grep -v '^$' | grep -vxF -f "$here/win95-imports.txt" || true)
    n=$(printf '%s\n' "$imports" | grep -c . || true)
    for k in MajorOSystemVersion MajorSubsystemVersion; do
        v=$(printf '%s\n' "$dump" | awk -v k=$k '$1 == k {print $2}')
        if [ "$v" != "4" ]; then echo "$f: $k is $v, must be 4"; status=1; fi
    done
    if [ -n "$bad" ]; then
        echo "$f: imports not allowed (not in Windows 95 and 2000, load them with GetProcAddress):"
        printf '  %s\n' $bad | paste - -
        status=1
    else
        echo "$f: $n imports, all in the Windows 95/2000 allow-list; OS/subsystem version 4.0"
    fi
done
exit $status
