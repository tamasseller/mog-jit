#!/usr/bin/env bash
# Reports, per arena size, whether a seed actually reaches eviction --
# see probe_arena.cpp's own header comment. Not part of a fuzzing run.
#
#   ./probe_arena.sh seeds/*
#
# -m32 for the same reason build.sh is: Runtime addresses its arena and
# every ProcSlot::bodyPtr as a bare uint32_t.
set -euo pipefail
cd "$(dirname "$0")"

g++ -std=c++17 -O1 -g -m32 -I ../src/compiler -I ../src/runtime \
    probe_arena.cpp \
    ../src/compiler/{ext,window,accstate,assembler,arithmetic,shape,abi_strategy,decode_instr,proc_scan,translate_proc,translate_data_flow,translate_control_flow}.cpp \
    ../src/runtime/runtime.cpp \
    -o probe_arena

./probe_arena "$@"
