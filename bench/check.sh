#!/usr/bin/env bash
# The sample-stream extension's acceptance gate.
#
# Generates one program plus its input samples from the reference half, links
# them into a bare-metal image with the real translator and the real unmodified
# runtime, runs the emitted Thumb under QEMU, and compares every value it
# touched against what @ppl/machine's VM computed for the same program.
set -euo pipefail
cd "$(dirname "$0")"

OUT_DIR="${BENCH_OUT_DIR:-${TMPDIR:-/tmp}/ppl-bench}"
ELF="$OUT_DIR/check.elf"
OUT="${TMPDIR:-/tmp}/ppl-bench-check.out"

mkdir -p "$OUT_DIR" generated
npx ts-node --transpile-only ts/gen-check.ts generated

make -s -C src/check OUT_DIR="$OUT_DIR"

# Semihosting output arrives on stderr under target=native, so both streams
# are kept (fuzz/qemu_exec/qemu_exec.ts:227-233 documents the same).
qemu-system-arm -M microbit -nographic -monitor none -serial none \
    -semihosting-config enable=on,target=native \
    -kernel "$ELF" > "$OUT" 2>&1 || true

npx ts-node --transpile-only ts/compare-check.ts generated/check_expected.json "$OUT"
