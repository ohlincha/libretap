#!/bin/sh
# Linux portability check: tick must be a static binary (runs on any x86-64 Linux,
# no libraries needed); libtickhook.so may only use libc symbols that every glibc
# since 2.2.5 has (getenv, __errno_location).
set -e
tick=$1 so=$2 status=0
if file "$tick" | grep -q "statically linked"; then echo "$tick: static, x86-64"; else echo "$tick: NOT static"; status=1; fi
bad=$(objdump -T "$so" | awk '/\*UND\*/ {print $NF, $(NF-1)}' | grep -vE '^(getenv|__errno_location) \(GLIBC_2\.2\.5\)$' || true)
if [ -n "$bad" ]; then echo "$so: unexpected imports:"; echo "$bad"; status=1
else echo "$so: imports only getenv and __errno_location (GLIBC_2.2.5)"; fi
need=$(objdump -p "$so" | awk '/NEEDED/ {print $2}' | tr '\n' ' ')
echo "$so: needs $need"
exit $status
