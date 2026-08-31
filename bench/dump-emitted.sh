#!/usr/bin/env bash
# Translate one sample-stream program on the host and disassemble what the
# translator emitted — fuzz/dump_code.sh with this extension linked in place
# of the weak defaults, plus a stub input buffer so it links.
#
# This is the fast gate on ext_sampstream.cpp: an emitter that overruns its
# declared halfword budget bails with RESOURCE_PROGRAM_EXT_UNSUPPORTED
# rather than emitting anything, and a wrong encoding is visible here long
# before a QEMU run would attribute it to the wrong thing.
set -euo pipefail
cd "$(dirname "$0")"

OUT="${TMPDIR:-/tmp}/ppl-bench-code"
ENVELOPE="${TMPDIR:-/tmp}/ppl-bench-envelope.bin"

npx ts-node --transpile-only gen-envelope.ts "$ENVELOPE" "${1:-pulse-trigger}"

cat > "${TMPDIR:-/tmp}/ppl-bench-samples-stub.cpp" <<'EOF'
#include "ext_sampstream.h"
const int16_t g_sampIn[SAMP_IN_SAMPLES] = {};
EOF

g++ -std=c++17 -O1 -g -m32 -I ../src/compiler -I ../src/runtime -I . \
    ../fuzz/dump_code.cpp \
    ext_sampstream.cpp "${TMPDIR:-/tmp}/ppl-bench-samples-stub.cpp" \
    ../src/compiler/{ext,ext_default,window,accstate,assembler,arithmetic,shape,abi_strategy,decode_instr,proc_scan,translate_proc,translate_data_flow,translate_control_flow}.cpp \
    ../src/runtime/runtime.cpp \
    -o "${TMPDIR:-/tmp}/ppl-bench-dump_code"

"${TMPDIR:-/tmp}/ppl-bench-dump_code" "$ENVELOPE" "$OUT"

for b in "$OUT".proc*.bin; do
    echo "=== $b ==="
    arm-none-eabi-objdump -D -b binary -m armv6-m -M force-thumb "$b" | tail -n +7
done
