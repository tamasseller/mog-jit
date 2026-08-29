#!/usr/bin/env bash
# Replays one saved input through the exact harness the fuzzer runs --
# same sanitizers, same two passes, one shot instead of a mutation loop.
#
#   ./repro.sh last_input.bin
set -euo pipefail
cd "$(dirname "$0")"

g++ -std=c++17 -O1 -g \
    -fsanitize=address,undefined -fno-sanitize-recover=all \
    -fno-sanitize=shift-base \
    -m32 \
    -DPPL_FUZZ_LIBFUZZER_BUILD \
    -I ../compiler/src -I ../runtime \
    repro.cpp \
    harness.cpp \
    ../compiler/src/{ext,window,accstate,assembler,arithmetic,shape,abi_strategy,decode_instr,proc_scan,translate_proc,translate_data_flow,translate_control_flow}.cpp \
    ../runtime/runtime.cpp \
    -o repro_driver

./repro_driver "$@"
