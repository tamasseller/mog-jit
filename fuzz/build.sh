#!/usr/bin/env bash
# Builds the fuzz harness with ASan+UBSan: harness.cpp's own main(), a dumb
# mutation loop with no coverage feedback, needing nothing but g++.
# build_afl.sh is the coverage-guided build of the same entry point.
# -m32: Runtime addresses its arena and every ProcSlot::bodyHandle as a bare
# uint32_t, so the host build has to be one where a real address fits.
set -euo pipefail
cd "$(dirname "$0")"

JIT_MK=(../src/compiler/compiler-all.mk ../src/compiler/emit/ext-default.mk ../src/runtime/runtime-all.mk ../src/runtime/bytecode-default.mk)
JIT_SOURCES=($(../tools/srclist.sh "${JIT_MK[@]}"))
JIT_INCLUDES=($(../tools/srclist.sh --includes "${JIT_MK[@]}"))

g++ -std=c++17 -O1 -g \
    -fsanitize=address,undefined -fno-sanitize-recover=all \
    -fno-sanitize=shift-base \
    -m32 \
    "${JIT_INCLUDES[@]}" -I ../test \
    harness.cpp \
    "${JIT_SOURCES[@]}" \
    ../test/ext_rawmem.cpp \
    rawmem_helper_host.cpp \
    -o fuzz_driver

echo "built ./fuzz_driver"
