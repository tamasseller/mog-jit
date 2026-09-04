#!/usr/bin/env bash
# Translate one sample-stream program on the host and disassemble what the
# translator emitted.
#
# This is the fast gate on ext_sampstream.cpp: an emitter that overruns its
# declared halfword budget bails with RESOURCE_PROGRAM_EXT_UNSUPPORTED rather
# than emitting anything, and a wrong encoding is visible here long before a
# QEMU run would attribute it to the wrong thing.
set -euo pipefail
cd "$(dirname "$0")"

OUT="${TMPDIR:-/tmp}/ppl-bench-code"
ENVELOPE="${TMPDIR:-/tmp}/ppl-bench-envelope.bin"

npx ts-node --transpile-only ts/gen-envelope.ts "$ENVELOPE" "${1:-pulse-trigger}"

make -s -C src/emitted
src/emitted/dump_emitted "$ENVELOPE" "$OUT"

for b in "$OUT".proc*.bin; do
    echo "=== $b ==="
    arm-none-eabi-objdump -D -b binary -m armv6-m -M force-thumb "$b" | tail -n +7
done
