#!/usr/bin/env bash
# Generates each workload's data and builds one benchmark image per
# optimization level from src/bench/Makefile.
set -euo pipefail
cd "$(dirname "$0")"

LEVELS=(O0 O1 O2 O3 Os Og)
WORKLOADS=("$@")
if [ ${#WORKLOADS[@]} -eq 0 ]; then
    WORKLOADS=(pulse-trigger iq-preamble median5)
fi

OUT_DIR="${BENCH_OUT_DIR:-${TMPDIR:-/tmp}/ppl-bench}"
mkdir -p "$OUT_DIR" generated

# One image per (workload, level). Each workload gets its own input samples
# rather than sharing one array: what fraction of each branch is taken is the
# dominant term in every number here, so a signal compromised to suit three
# workloads at once would make all three less meaningful.
for workload in "${WORKLOADS[@]}"; do
    npx ts-node --transpile-only ts/gen-bench.ts generated "$workload"

    for level in "${LEVELS[@]}"; do
        make -s -C src/bench WORKLOAD="$workload" LEVEL="$level" OUT_DIR="$OUT_DIR" > /dev/null

        # bench.ts reads the reference kernels' frame size from GCC's own .su,
        # which lands beside the object rather than beside the image.
        cp "$(find "src/bench/.o/$workload/$level" -name 'kernels_ref.cpp.su')" \
            "$OUT_DIR/kernels_ref.$workload.$level.su"

        printf '  %-14s %-3s %s bytes text\n' "$workload" "$level" \
            "$(arm-none-eabi-size "$OUT_DIR/bench.$workload.$level.elf" | tail -1 | awk '{print $1}')"
    done
done

echo "built $(( ${#LEVELS[@]} * ${#WORKLOADS[@]} )) images in $OUT_DIR"
