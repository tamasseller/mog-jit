#!/usr/bin/env bash
# Builds one benchmark image per optimization level.
#
# Exactly one thing varies across the six: the level kernels_ref.cpp is
# compiled at. The runtime, the translator and the runner itself stay at the
# -Os they ship at in every image, so the JIT-side numbers must come out
# identical across all six — which the driver checks, and which is a free
# consistency test on the whole measurement.
#
# -fstack-usage on the reference kernels only; the driver reads the .su
# files for the C-side stack figure. The JIT side has no compile-time
# equivalent — its stack is a property of the program it is running, not of
# any function GCC ever saw — so that number comes from the validator and
# the runtime watermark instead.
#
# A plain script rather than an ultimate-makefile target, matching fuzz/'s
# images: one link of a fixed source list, no test framework, no coverage.
set -euo pipefail
cd "$(dirname "$0")"

QEMU_EXEC=../fuzz/qemu_exec
OUT_DIR="${BENCH_OUT_DIR:-${TMPDIR:-/tmp}/ppl-bench}"
LEVELS=(-O0 -O1 -O2 -O3 -Os -Og)

mkdir -p "$OUT_DIR" generated

npx ts-node --transpile-only gen-bench.ts generated

# Kept in step with test/qemu/Makefile's. The -ffixed-r8..r11 reservations
# and -DNDEBUG are the two that matter: runtime.S and every
# `register ... asm("r9")` site depend on the allocator never touching
# those, and a firing assert() would pull in newlib's fprintf path.
COMMON=(
    -mcpu=cortex-m0 -mthumb -DNDEBUG
    -std=gnu++17 -fno-exceptions -fno-rtti
    -ffixed-r8 -ffixed-r9 -ffixed-r10 -ffixed-r11
    -fno-use-cxa-atexit
    -ffunction-sections
    -I ../src/compiler -I ../src/runtime -I . -I "$QEMU_EXEC"
)

FIXED_SOURCES=(
    "$QEMU_EXEC/vectors.S"
    ../src/runtime/runtime.S
    ../src/runtime/runtime.cpp
    ../src/runtime/executor.cpp
    ../src/runtime/dispatch_abi.cpp
    ../src/compiler/window.cpp
    ../src/compiler/ext.cpp
    ../src/compiler/ext_default.cpp
    ../src/compiler/accstate.cpp
    ../src/compiler/arithmetic.cpp
    ../src/compiler/assembler.cpp
    ../src/compiler/shape.cpp
    ../src/compiler/abi_strategy.cpp
    ../src/compiler/decode_instr.cpp
    ../src/compiler/proc_scan.cpp
    ../src/compiler/translate_proc.cpp
    ../src/compiler/translate_data_flow.cpp
    ../src/compiler/translate_control_flow.cpp
    "$QEMU_EXEC/semihost.cpp"
    "$QEMU_EXEC/cxx_stubs.cpp"
    ext_sampstream.cpp
    bench_stack.cpp
    generated/samples.cpp
    generated/bench_data.cpp
    bench_runner.cpp
)

for level in "${LEVELS[@]}"; do
    tag="${level#-}"
    obj="$OUT_DIR/kernels_ref.$tag.o"
    elf="$OUT_DIR/bench.$tag.elf"

    # The reference kernels are the only thing whose level moves, and they
    # are compiled separately so the .su file is attributable to them alone.
    arm-none-eabi-g++ "${COMMON[@]}" "$level" -fstack-usage \
        -c kernels_ref.cpp -o "$obj" # .su lands beside the object

    # vectors.S first on the link line — nothing else pins the vector table
    # to address 0.
    arm-none-eabi-g++ "${COMMON[@]}" -Os \
        -static -nostartfiles -specs=nosys.specs -T linker.ld \
        "${FIXED_SOURCES[@]}" "$obj" \
        -o "$elf"

    printf '%-6s %s\n' "$tag" "$(arm-none-eabi-size "$elf" | tail -1)"
done

echo "built ${#LEVELS[@]} images in $OUT_DIR"
