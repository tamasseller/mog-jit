#!/usr/bin/env bash
# Translate one program and disassemble what the translator emitted.
set -euo pipefail
cd "$(dirname "$0")"
g++ -std=c++17 -O1 -g -m32 -I ../src/compiler -I ../src/runtime dump_code.cpp \
    ../src/compiler/{ext,ext_default,window,accstate,assembler,arithmetic,shape,abi_strategy,decode_instr,proc_scan,translate_proc,translate_data_flow,translate_control_flow}.cpp \
    ../src/runtime/runtime.cpp \
    -o dump_code
./dump_code "$1" /tmp/ppl-code
for b in /tmp/ppl-code.proc*.bin; do
    echo "=== $b ==="
    arm-none-eabi-objdump -D -b binary -m armv6-m -M force-thumb "$b" | tail -n +7
done
