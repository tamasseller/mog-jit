#!/usr/bin/env bash
# Builds the fuzz harness with ASan+UBSan: harness.cpp's own main(), a dumb
# mutation loop with no coverage feedback, needing nothing but g++.
# build_afl.sh is the coverage-guided build of the same entry point.
# -m32: Runtime addresses its arena and every ProcSlot::bodyHandle as a bare
# uint32_t, so the host build has to be one where a real address fits.
set -euo pipefail
cd "$(dirname "$0")"

g++ -std=c++17 -O1 -g \
    -fsanitize=address,undefined -fno-sanitize-recover=all \
    -fno-sanitize=shift-base \
    -m32 \
    -I ../src/compiler -I ../src/runtime -I ../test \
    harness.cpp \
    ../src/compiler/window.cpp \
    ../src/compiler/ext.cpp \
    ../src/compiler/ext_default.cpp \
    ../test/ext_rawmem.cpp \
    rawmem_helper_host.cpp \
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
    ../src/runtime/bytecode_default.cpp \
    -o fuzz_driver

echo "built ./fuzz_driver"
