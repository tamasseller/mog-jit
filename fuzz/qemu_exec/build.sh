#!/usr/bin/env bash
# Builds the execution-oracle image (exec_runner.cpp) — the real translator
# plus the real, unmodified runtime/, linked for the same lm3s811evb model
# test/qemu targets.
#
# A plain script rather than an ultimate-makefile target, matching the rest
# of fuzz/ (build.sh, build_afl.sh): this image has no test-framework
# dependency, no coverage step and no stack-usage tracking of its own — it
# is one link of a fixed source list. Flags are kept in step with
# test/qemu/Makefile's, since the whole point of the oracle is that the
# code under test is byte-for-byte what that suite already validates on
# emulated hardware; the two that matter most are the -ffixed-r8..r11
# reservations (runtime.S and every `register ... asm("r9")` site depend on
# the allocator never touching those) and -DNDEBUG (a firing assert() would
# pull in newlib's fprintf path and a heap this design has no room for).
set -euo pipefail
cd "$(dirname "$0")"

CXXFLAGS=(
    -mcpu=cortex-m0 -mthumb -Os -DNDEBUG
    -std=gnu++17 -fno-exceptions -fno-rtti
    -ffixed-r8 -ffixed-r9 -ffixed-r10 -ffixed-r11
    -fno-use-cxa-atexit
    -I ../../src/compiler -I ../../src/runtime -I .
)

# vectors.S first on the link line — nothing else pins the vector table to
# address 0.
arm-none-eabi-g++ "${CXXFLAGS[@]}" \
    -static -nostartfiles -specs=nosys.specs -T linker.ld \
    vectors.S \
    ../../src/runtime/runtime.S \
    ../../src/runtime/runtime.cpp \
    ../../src/runtime/enter_program.cpp \
    ../../src/runtime/dispatch_abi.cpp \
    ../../src/compiler/window.cpp \
    ../../src/compiler/ext.cpp \
    ../../src/compiler/accstate.cpp \
    ../../src/compiler/arithmetic.cpp \
    ../../src/compiler/assembler.cpp \
    ../../src/compiler/shape.cpp \
    ../../src/compiler/abi_strategy.cpp \
    ../../src/compiler/decode_instr.cpp \
    ../../src/compiler/proc_scan.cpp \
    ../../src/compiler/translate_proc.cpp \
    ../../src/compiler/translate_data_flow.cpp \
    ../../src/compiler/translate_control_flow.cpp \
    semihost.cpp \
    cxx_stubs.cpp \
    exec_runner.cpp \
    -o exec_runner.elf

arm-none-eabi-size exec_runner.elf
echo "built ./exec_runner.elf"
