#!/usr/bin/env bash
# Translate one program and disassemble what the translator emitted.
set -euo pipefail
cd "$(dirname "$0")"

JIT_MK=(../src/compiler/compiler-all.mk ../src/compiler/emit/ext-default.mk ../src/runtime/runtime-all.mk ../src/runtime/bytecode-default.mk)
JIT_SOURCES=($(../tools/srclist.sh "${JIT_MK[@]}"))
JIT_INCLUDES=($(../tools/srclist.sh --includes "${JIT_MK[@]}"))
g++ -std=c++17 -O1 -g -m32 "${JIT_INCLUDES[@]}" dump_code.cpp \
    "${JIT_SOURCES[@]}" \
    -o dump_code
./dump_code "$1" /tmp/ppl-code
for b in /tmp/ppl-code.proc*.bin; do
    echo "=== $b ==="
    arm-none-eabi-objdump -D -b binary -m armv6-m -M force-thumb "$b" | tail -n +7
done
