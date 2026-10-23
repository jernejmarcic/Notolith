#!/bin/sh
# usage: check_needed.sh <elf> <pattern>...
# Fails when <elf> lists a NEEDED library matching one of the patterns (grep -E, case-insensitive), or has a RUNPATH
# with an empty entry (the loader reads an empty entry as the current directory). Deterministic, no process started.
elf=$1
shift
dynamic=$(readelf -d "$elf") || exit 2
needed=$(printf '%s\n' "$dynamic" | grep NEEDED)
status=0
for pattern in "$@"; do
    hit=$(printf '%s\n' "$needed" | grep -i -E -- "$pattern")
    if [ -n "$hit" ]; then
        echo "$elf NEEDs a library matching '$pattern':"
        echo "$hit"
        status=1
    fi
done
runpath=$(printf '%s\n' "$dynamic" | grep -E 'RUNPATH|RPATH')
if printf '%s\n' "$runpath" | grep -q -E '\[(:.*|.*::.*|.*:)\]'; then
    echo "$elf has an empty RUNPATH entry: $runpath"
    status=1
fi
exit $status
