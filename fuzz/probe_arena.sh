#!/usr/bin/env bash
# Reports, per arena size, whether a seed actually reaches eviction --
# see probe_arena.cpp's own header comment. Not part of a fuzzing run.
#
#   ./probe_arena.sh seeds/*
#
# -m32 for the same reason build.sh is: Runtime addresses its arena and
# every ProcSlot::bodyHandle as a bare uint32_t.
set -euo pipefail
cd "$(dirname "$0")"

JIT_MK=(../src/compiler/compiler-all.mk ../src/compiler/emit/ext-default.mk ../src/runtime/runtime-all.mk ../src/runtime/bytecode-default.mk)
JIT_SOURCES=($(../tools/srclist.sh "${JIT_MK[@]}"))
JIT_INCLUDES=($(../tools/srclist.sh --includes "${JIT_MK[@]}"))

g++ -std=c++17 -O1 -g -m32 "${JIT_INCLUDES[@]}" \
    probe_arena.cpp \
    "${JIT_SOURCES[@]}" \
    -o probe_arena

./probe_arena "$@"
