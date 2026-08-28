#!/usr/bin/env bash
# Builds the fuzz harness with ASan+UBSan. No clang/AFL++ available in this
# environment, so this is a plain g++ build of the dumb mutation-loop
# driver (harness.cpp's own main()) -- swap in
# `clang++ -fsanitize=fuzzer,address,undefined -DPPL_FUZZ_LIBFUZZER_BUILD`
# once libFuzzer or afl-clang-fast is installed; harness.cpp needs no
# changes for that, only the compiler invocation does.
set -euo pipefail
cd "$(dirname "$0")"

g++ -std=c++17 -O1 -g \
    -fsanitize=address,undefined -fno-sanitize-recover=all \
    -fno-sanitize=shift-base \
    -I ../compiler/src -I ../runtime \
    harness.cpp \
    ../compiler/src/window.cpp \
    ../compiler/src/ext.cpp \
    ../compiler/src/accstate.cpp \
    ../compiler/src/binops.cpp \
    ../compiler/src/assembler.cpp \
    ../compiler/src/shape.cpp \
    ../compiler/src/abi_strategy.cpp \
    ../compiler/src/decode_instr.cpp \
    ../compiler/src/blocks.cpp \
    ../compiler/src/unaryops.cpp \
    ../compiler/src/proc_scan.cpp \
    ../compiler/src/translate_proc.cpp \
    -o fuzz_driver

echo "built ./fuzz_driver"
