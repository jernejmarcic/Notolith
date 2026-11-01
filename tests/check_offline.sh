#!/bin/sh
# usage: check_offline.sh <needed-pattern> <symbol-pattern> <elf-or-directory>...
# Fails when an ELF file (a directory stands for the *.so in it) lists a NEEDED library matching <needed-pattern>, or
# names a dynamic symbol matching <symbol-pattern> (both grep -E; an empty pattern skips that check). The default
# build must have no networking and no encryption (docs/OFFLINE.md); this is the link-time half of that, with no
# process started.
needed_pattern=$1
symbol_pattern=$2
shift 2
status=0
for target in "$@"; do
    if [ -d "$target" ]; then
        files=$(ls "$target"/*.so 2>/dev/null)
    else
        files=$target
    fi
    for elf in $files; do
        if [ -n "$needed_pattern" ]; then
            hit=$(readelf -d "$elf" | grep NEEDED | grep -E -- "$needed_pattern")
            if [ -n "$hit" ]; then
                echo "$elf NEEDs a library matching '$needed_pattern':"
                echo "$hit"
                status=1
            fi
        fi
        if [ -n "$symbol_pattern" ]; then
            hit=$(nm -D "$elf" | grep -E -- "$symbol_pattern" | head -5)
            if [ -n "$hit" ]; then
                echo "$elf names a symbol matching '$symbol_pattern':"
                echo "$hit"
                status=1
            fi
        fi
    done
done
exit $status
