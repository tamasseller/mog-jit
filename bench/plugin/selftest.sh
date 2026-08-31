#!/usr/bin/env bash
# Build and run the plugin self-test: a region around a loop of exactly
# known length, against a region around nothing. Fails loudly if the
# difference is not what the assembly says it must be.
#
# Nothing here should be believed about the benchmark numbers until this
# passes — a counter that is quietly wrong is worse than no counter.
set -euo pipefail
cd "$(dirname "$0")"

QEMU_EXEC=../../fuzz/qemu_exec
ELF="${TMPDIR:-/tmp}/ppl-bench-plugin-selftest.elf"
LOG="${TMPDIR:-/tmp}/ppl-bench-plugin-selftest.log"

./build.sh > /dev/null

arm-none-eabi-g++ \
    -mcpu=cortex-m0 -mthumb -Os -DNDEBUG \
    -std=gnu++17 -fno-exceptions -fno-rtti -fno-use-cxa-atexit \
    -I "$QEMU_EXEC" -I .. \
    -static -nostartfiles -specs=nosys.specs -T "$QEMU_EXEC/linker.ld" \
    "$QEMU_EXEC/vectors.S" \
    "$QEMU_EXEC/semihost.cpp" \
    "$QEMU_EXEC/cxx_stubs.cpp" \
    selftest.cpp \
    -o "$ELF"

addr() { arm-none-eabi-nm "$ELF" | awk -v s="$1" '$3 == s {print $1}'; }

REGIONS=(
    "region=calibration:$(addr bench_enter_calibration):$(addr bench_exit_calibration)"
    "region=knownloop:$(addr bench_enter_knownloop):$(addr bench_exit_knownloop)"
)

PLUGIN_ARG="$(pwd)/bench_plugin.so"
for r in "${REGIONS[@]}"; do PLUGIN_ARG="$PLUGIN_ARG,$r"; done
PLUGIN_ARG="$PLUGIN_ARG,cycles=on,out=$LOG"

qemu-system-arm -M microbit -nographic -monitor none -serial none \
    -semihosting-config enable=on,target=native \
    -plugin "$PLUGIN_ARG" \
    -kernel "$ELF" > /dev/null 2>&1 || true

cat "$LOG"

# 200 iterations of `subs`/`bne`, plus the setup `movs`:
#   instructions  1 + 2*200                          = 401
#   cycles        1 + 200*1 + 199*3 (taken) + 1*1    = 799
# The marker pairs are identical in both regions, so differencing removes
# them from each figure without either having to be known.
awk '
function val(field,   i, v) {
    for(i = 1; i <= NF; i++) if($i ~ "^" field "=") { v = $i; sub(field "=", "", v); return v }
    return ""
}
/^REGION calibration/ { calI = val("insns"); calC = val("cycles") }
/^REGION knownloop/   { loopI = val("insns"); loopC = val("cycles") }
END {
    if(calI == "" || loopI == "") { print "FAIL: a region never reported"; exit 1 }

    di = loopI - calI
    dc = loopC - calC
    printf "instructions: %d (expected 401)\n", di
    printf "cycles:       %d (expected 799)\n", dc

    if(di != 401) { print "FAIL: the instruction counter is not counting what it claims"; exit 1 }
    if(dc != 799) { print "FAIL: the cycle model is not counting what it claims"; exit 1 }
    print "PASS"
}' "$LOG"
