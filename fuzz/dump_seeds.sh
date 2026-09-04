#!/usr/bin/env bash
# Stages ../test/corpus_programs.h's bodies into seeds_raw/ -- see
# dump_seeds.cpp's own header comment. Not part of the fuzzing run itself
# (build.sh/build_afl.sh). Run this after changing corpus_programs.h, then
# make_seeds.ts, which is what actually writes seeds/:
#
#   ./dump_seeds.sh
#   TS_NODE_PROJECT=tsconfig.json npx ts-node --transpile-only make_seeds.ts
set -euo pipefail
cd "$(dirname "$0")"

JIT_MK=(../src/compiler/compiler-headers.mk ../src/runtime/runtime-headers.mk ../src/runtime/bytecode-default.mk)
JIT_SOURCES=($(../tools/srclist.sh "${JIT_MK[@]}"))
JIT_INCLUDES=($(../tools/srclist.sh --includes "${JIT_MK[@]}"))

g++ -std=c++17 -O1 \
    "${JIT_INCLUDES[@]}" -I ../test \
    dump_seeds.cpp \
    ../test/encode_instr.cpp \
    "${JIT_SOURCES[@]}" \
    -o dump_seeds

mkdir -p seeds_raw
./dump_seeds
