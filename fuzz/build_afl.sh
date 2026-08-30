#!/usr/bin/env bash
# Real coverage-guided build: afl-clang-fast++ (LLVM/SanitizerCoverage
# instrumentation; the GCC_PLUGIN backend via afl-g++-fast fails to load on
# this machine -- prebuilt plugin vs. installed gcc ABI mismatch).
#
# ASan is on by default for AFL builds (AFL_USE_ASAN=1) -- catches the same
# class of bug build.sh's plain ASan build does, but now under real
# edge-coverage-guided mutation instead of a dumb byte-flip loop.
#
# UBSan is added manually rather than via AFL_USE_UBSAN=1: that variable
# makes afl-cc pass -fsanitize-undefined-trap-on-error, which turns any UB
# into a bare SIGILL with zero diagnostic text -- afl-fuzz still saves the
# crashing input, but root-causing it means a whole separate rebuild+replay
# cycle just to get a report. Plain (non-trap) UBSan costs nothing extra
# except when a crash actually happens, which is exactly when the extra
# output is worth it.
# -m32: Runtime addresses its arena and every ProcSlot::bodyPtr as a bare
# uint32_t, so the host build has to be one where a real address fits.
set -euo pipefail
cd "$(dirname "$0")"

export AFL_USE_ASAN=1

afl-clang-fast++ -std=c++17 -O1 -g \
    -fsanitize=undefined -fno-sanitize-recover=all \
    -fno-sanitize=shift-base \
    -m32 \
    -I ../src/compiler -I ../src/runtime \
    harness.cpp \
    ../src/compiler/window.cpp \
    ../src/compiler/ext.cpp \
    ../src/compiler/ext_default.cpp \
    ../src/compiler/accstate.cpp \
    ../src/compiler/assembler.cpp \
    ../src/compiler/arithmetic.cpp \
    ../src/compiler/shape.cpp \
    ../src/compiler/abi_strategy.cpp \
    ../src/compiler/decode_instr.cpp \
    ../src/compiler/proc_scan.cpp \
    ../src/compiler/translate_proc.cpp \
    ../src/compiler/translate_data_flow.cpp \
    ../src/compiler/translate_control_flow.cpp \
    ../src/runtime/runtime.cpp \
    -o fuzz_driver_afl

echo "built ./fuzz_driver_afl"
