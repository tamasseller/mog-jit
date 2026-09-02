#!/usr/bin/env bash
# The sample-stream extension's acceptance gate.
#
# Generates one program plus its input samples from the reference half,
# links them into a bare-metal image with the real translator and the real
# unmodified runtime, runs the emitted Thumb under QEMU, and compares every
# value it touched against what @ppl/machine's VM computed for the same
# program.
#
# A plain script rather than an ultimate-makefile target, matching fuzz/'s
# own images (see fuzz/qemu_exec/build.sh's header): one link of a fixed
# source list, no test framework, no coverage, no stack-usage tracking.
# Flags are kept in step with test/qemu/Makefile's — the -ffixed-r8..r11
# reservations and -DNDEBUG are the two that matter.
set -euo pipefail
cd "$(dirname "$0")"

QEMU_EXEC=../fuzz/qemu_exec
ELF="${TMPDIR:-/tmp}/ppl-bench-check.elf"
OUT="${TMPDIR:-/tmp}/ppl-bench-check.out"

mkdir -p generated
npx ts-node --transpile-only gen-check.ts generated

CXXFLAGS=(
    -mcpu=cortex-m0 -mthumb -Os -DNDEBUG
    -std=gnu++17 -fno-exceptions -fno-rtti
    -ffixed-r8 -ffixed-r9 -ffixed-r10 -ffixed-r11
    -fno-use-cxa-atexit
    -I ../src/compiler -I ../src/runtime -I . -I "$QEMU_EXEC"
)

# vectors.S first on the link line — nothing else pins the vector table to
# address 0.
arm-none-eabi-g++ "${CXXFLAGS[@]}" \
    -static -nostartfiles -specs=nosys.specs -T linker.ld \
    "$QEMU_EXEC/vectors.S" \
    ../src/runtime/runtime.S \
    ../src/runtime/runtime.cpp \
    ../src/runtime/bytecode_default.cpp \
    ../src/runtime/executor.cpp \
    ../src/runtime/dispatch_abi.cpp \
    ../src/compiler/window.cpp \
    ../src/compiler/ext.cpp \
    ../src/compiler/ext_default.cpp \
    ../src/compiler/accstate.cpp \
    ../src/compiler/arithmetic.cpp \
    ../src/compiler/assembler.cpp \
    ../src/compiler/shape.cpp \
    ../src/compiler/abi_strategy.cpp \
    ../src/compiler/decode_instr.cpp \
    ../src/compiler/proc_scan.cpp \
    ../src/compiler/translate_proc.cpp \
    ../src/compiler/translate_data_flow.cpp \
    ../src/compiler/translate_control_flow.cpp \
    "$QEMU_EXEC/semihost.cpp" \
    "$QEMU_EXEC/cxx_stubs.cpp" \
    ext_sampstream.cpp \
    generated/check_samples.cpp \
    generated/check_data.cpp \
    check_runner.cpp \
    -o "$ELF"

arm-none-eabi-size "$ELF"

# Semihosting output arrives on stderr under target=native, so both streams
# are kept (fuzz/qemu_exec/qemu_exec.ts:227-233 documents the same).
qemu-system-arm -M microbit -nographic -monitor none -serial none \
    -semihosting-config enable=on,target=native \
    -kernel "$ELF" > "$OUT" 2>&1 || true

npx ts-node --transpile-only compare-check.ts generated/check_expected.json "$OUT"
