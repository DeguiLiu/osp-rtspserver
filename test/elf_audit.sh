#!/usr/bin/env bash
# elf_audit.sh -- assert that the server library pulls in no forbidden symbols.
#
# The RTSP server is built with -fno-exceptions -fno-rtti and is meant to be
# allocation-free on the media path. Those properties are easy to break by
# accident (a std::string, a std::vector, a libatomic fallback for a 64-bit
# atomic on 32-bit ARM), and a code review alone does not catch them. This
# script scans the undefined symbols of the built objects and fails when a
# forbidden one appears.
#
# Usage:
#   test/elf_audit.sh <nm-tool> <object-or-ELF>...
#   e.g. test/elf_audit.sh nm build/CMakeFiles/rs_core.dir/src/*.o
#
# Exit 0 when every input is clean; exit 1 and print the offending symbol
# otherwise; exit 2 on a usage error or a missing input.
#
# SPDX-License-Identifier: MIT
set -u

NM="${1:?usage: $0 <nm-tool> <object...>}"
shift

if [ "$#" -eq 0 ]; then
    echo "usage: $0 <nm-tool> <object...>" >&2
    exit 2
fi

# Forbidden symbol classes:
#   malloc family   - the media path must not allocate
#   operator new    - same, for the C++ form
#   __cxa_throw     - exceptions are compiled out; a reference means the
#                     -fno-exceptions flag was lost
#   __dynamic_cast  - RTTI is compiled out; same reasoning
#   __atomic_*      - libatomic fallback for a non-lock-free atomic. On armhf
#                     a 64-bit atomic silently pulls in a lock plus a heap
#                     allocation, which is exactly what this audit exists to
#                     catch.
#   __sync_*        - the older libgcc sync builtins, same class of problem
BANNED_RE='(^|[^_[:alnum:]])(malloc|calloc|realloc|free)([^_[:alnum:]]|$)|operator[[:space:]]+(new|delete)|__cxa_throw|__cxa_allocate_exception|__cxa_free_exception|__dynamic_cast|__atomic_|__sync_'

rc=0
for obj in "$@"; do
    if [ ! -f "$obj" ]; then
        echo "MISSING: $obj" >&2
        rc=1
        continue
    fi
    hits=$("$NM" -u --demangle "$obj" 2>/dev/null | grep -E "$BANNED_RE" || true)
    if [ -n "$hits" ]; then
        echo "HIT: $obj"
        echo "$hits"
        rc=1
    else
        echo "CLEAN: $obj"
    fi
done

if [ "$rc" -eq 0 ]; then
    echo "elf_audit: PASS (no banned symbols in $# object(s))"
else
    echo "elf_audit: FAIL (banned symbol(s) above)"
fi
exit "$rc"
