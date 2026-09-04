#!/usr/bin/env bash
# Replays one saved input through the exact harness the fuzzer runs --
# same sanitizers, same two passes, one shot instead of a mutation loop.
#
#   ./repro.sh last_input.bin
set -euo pipefail
cd "$(dirname "$0")"

JIT_MK=(../src/compiler/compiler-all.mk ../src/compiler/emit/ext-default.mk ../src/runtime/runtime-all.mk ../src/runtime/bytecode-default.mk)
JIT_SOURCES=($(../tools/srclist.sh "${JIT_MK[@]}"))
JIT_INCLUDES=($(../tools/srclist.sh --includes "${JIT_MK[@]}"))

g++ -std=c++17 -O1 -g \
    -fsanitize=address,undefined -fno-sanitize-recover=all \
    -fno-sanitize=shift-base \
    -m32 \
    -DPPL_FUZZ_LIBFUZZER_BUILD \
    "${JIT_INCLUDES[@]}" -I ../test \
    repro.cpp \
    harness.cpp \
    "${JIT_SOURCES[@]}" \
    ../test/ext_rawmem.cpp \
    rawmem_helper_host.cpp \
    -o repro_driver

./repro_driver "$@"
