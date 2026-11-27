#!/bin/sh
# DemoMeter/DemoInstrument: every import must be on Tick's Windows 95/2000 allow-list, or be one of the Winsock 2
# functions below (ws2_32.dll: Windows 98, NT 4 SP4, 2000, XP and later; Windows 95 with the Winsock 2 update).
# Usage: tools/check-demo-imports.sh OBJDUMP file.exe...
here=$(dirname "$0"); OBJDUMP=$1; shift; status=0
# Win32 functions since Windows 95 / NT 3.1 that Tick itself does not use (so not on its list):
extra="kernel32.dll|GetStdHandle kernel32.dll|SetCommState kernel32.dll|SetCommTimeouts kernel32.dll|SetConsoleTitleA user32.dll|IsDialogMessageA user32.dll|FindWindowA user32.dll|IsWindow user32.dll|SendMessageW user32.dll|wvsprintfA"
ws2="accept bind inet_addr closesocket connect htonl htons listen recv send setsockopt socket WSAStartup WSACleanup"
for f in "$@"; do
    imports=$("$OBJDUMP" -p "$f" | awk '/DLL Name:/ {d = tolower($3)} /^ +[0-9]+ +[A-Za-z_?@]/ && d {print d, $2}')
    for i in $(printf '%s\n' "$imports" | tr ' ' '|'); do
        d=${i%%|*}; fn=${i#*|}
        if [ "$d" = ws2_32.dll ]; then
            case " $ws2 " in *" $fn "*) ;; *) echo "$f: $d $fn not allowed"; status=1;; esac
        elif case " $extra " in *" $i "*) true;; *) false;; esac; then :
        elif ! grep -qxF "$d $fn" "$here/win95-imports.txt"; then echo "$f: $d $fn not allowed"; status=1; fi
    done
    [ $status = 0 ] && echo "$f: $(printf '%s\n' "$imports" | grep -c .) imports, all on Windows 98/2000/XP"
done
exit $status
