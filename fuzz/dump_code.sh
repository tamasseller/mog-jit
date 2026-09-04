#!/usr/bin/env bash
# Translate one program and disassemble what the translator emitted.
#
#   ./dump_code.sh seeds/arith
set -euo pipefail

# Resolve arguments before the cd, so a path relative to the caller's own
# directory still means what it said.
args=(); for a in "$@"; do args+=("$(realpath -m "$a")"); done
cd "$(dirname "$0")"

OUT="${TMPDIR:-/tmp}/ppl-code"

make -s -C src/dump-code
src/dump-code/dump_code "${args[0]}" "$OUT"

for b in "$OUT".proc*.bin; do
    echo "=== $b ==="
    arm-none-eabi-objdump -D -b binary -m armv6-m -M force-thumb "$b" | tail -n +7
done
