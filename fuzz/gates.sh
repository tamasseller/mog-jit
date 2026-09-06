#!/usr/bin/env bash
# Everything that has to hold before a campaign's findings mean anything.
# Each of these is a differential or a calibration, not a smoke test: if one
# fails, the campaign is comparing something against itself.
#
#   ./gates.sh
set -euo pipefail
cd "$(dirname "$0")"

run() { printf '\n=== %s\n' "$1"; shift; "$@"; }

make -s -C src/qemu-exec
make -s -C src/driver

run "AST evaluator against hand-picked answers"   npx ts-node --transpile-only ts/eval/eval_check.ts
run "UB analysis, both directions"                npx ts-node --transpile-only ts/gen/ub_check.ts
run "inner differential (evaluator vs reference VM)" npx ts-node --transpile-only ts/eval/diff.ts
run "generator survival"                          npx ts-node --transpile-only ts/gen/stats.ts
run "invalid lane (nothing must get past the host toolchain)" \
    npx ts-node --transpile-only ts/invalid.ts --rounds 500
run "frame lane (every damaged program refused on target)" \
    npx ts-node --transpile-only ts/frame.ts --rounds 300
run "regression corpus on target"                 npx ts-node --transpile-only ts/qemu-exec.ts seeds

printf '\nall gates passed\n'
