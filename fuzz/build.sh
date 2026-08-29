#!/usr/bin/env bash
# Builds the fuzz harness with ASan+UBSan. No clang/AFL++ available in this
# environment, so this is a plain g++ build of the dumb mutation-loop
# driver (harness.cpp's own main()) -- swap in
# `clang++ -fsanitize=fuzzer,address,undefined -DPPL_FUZZ_LIBFUZZER_BUILD`
# once libFuzzer or afl-clang-fast is installed; harness.cpp needs no
# changes for that, only the compiler invocation does.
# -m32: Runtime addresses its arena and every ProcSlot::bodyPtr as a bare
# uint32_t, so the host build has to be one where a real address fits.
set -euo pipefail
cd "$(dirname "$0")"

g++ -std=c++17 -O1 -g \
    -fsanitize=address,undefined -fno-sanitize-recover=all \
    -fno-sanitize=shift-base \
    -m32 \
    -I ../compiler/src -I ../runtime \
    harness.cpp \
    ../compiler/src/window.cpp \
    ../compiler/src/ext.cpp \
    ../compiler/src/accstate.cpp \
    ../compiler/src/assembler.cpp \
    ../compiler/src/arithmetic.cpp \
    ../compiler/src/shape.cpp \
    ../compiler/src/abi_strategy.cpp \
    ../compiler/src/decode_instr.cpp \
    ../compiler/src/proc_scan.cpp \
    ../compiler/src/translate_proc.cpp \
    ../compiler/src/translate_data_flow.cpp \
    ../compiler/src/translate_control_flow.cpp \
    ../runtime/runtime.cpp \
    -o fuzz_driver

echo "built ./fuzz_driver"
