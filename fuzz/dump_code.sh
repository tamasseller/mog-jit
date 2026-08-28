#!/usr/bin/env bash
# Translate one program and disassemble what the translator emitted.
set -euo pipefail
cd "$(dirname "$0")"
g++ -std=c++17 -O1 -g -I ../compiler/src -I ../runtime dump_code.cpp \
    ../compiler/src/{window,accstate,binops,assembler,shape,abi_strategy,decode_instr,blocks,unaryops,proc_scan,translate_proc}.cpp \
    -o dump_code
./dump_code "$1" /tmp/ppl-code
for b in /tmp/ppl-code.proc*.bin; do
    echo "=== $b ==="
    arm-none-eabi-objdump -D -b binary -m armv6-m -M force-thumb "$b" | tail -n +7
done
