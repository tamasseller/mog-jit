#!/usr/bin/env bash
# Builds the execution-oracle image (exec_runner.cpp) — the real translator
# plus the real, unmodified runtime/, linked for the same microbit model
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

JIT_MK=(../../src/compiler/compiler-all.mk ../../src/compiler/emit/ext-default.mk ../../src/runtime/runtime-all.mk ../../src/runtime/executor.mk ../../src/runtime/dispatch.mk ../../src/runtime/bytecode-default.mk)
JIT_SOURCES=($(../../tools/srclist.sh "${JIT_MK[@]}"))
JIT_INCLUDES=($(../../tools/srclist.sh --includes "${JIT_MK[@]}"))

CXXFLAGS=(
    -mcpu=cortex-m0 -mthumb -Os -DNDEBUG
    -std=gnu++17 -fno-exceptions -fno-rtti
    -ffixed-r8 -ffixed-r9 -ffixed-r10 -ffixed-r11
    -fno-use-cxa-atexit
    "${JIT_INCLUDES[@]}" -I ../../test -I .
)

# vectors.S first on the link line — nothing else pins the vector table to
# address 0.
arm-none-eabi-g++ "${CXXFLAGS[@]}" \
    -static -nostartfiles -specs=nosys.specs -T linker.ld \
    vectors.S \
    "${JIT_SOURCES[@]}" \
    ../../test/ext_rawmem.cpp \
    ../../test/ext_rawmem_helper.S \
    semihost.cpp \
    cxx_stubs.cpp \
    exec_runner.cpp \
    -o exec_runner.elf

arm-none-eabi-size exec_runner.elf
echo "built ./exec_runner.elf"
