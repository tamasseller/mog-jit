#!/usr/bin/env bash
# Replays one program through the exact harness the crash sink runs — the
# host translator under ASan/UBSan with asserts live.
#
# The driver reports a finding as `(corpus entry, seed)` rather than a file,
# so the program is written out first:
#
#   npx ts-node --transpile-only ts/gen/show.ts nested_loop 4242 --emit /tmp/p.bin
#   ./repro.sh /tmp/p.bin
set -euo pipefail

# Resolve arguments before the cd, so a path relative to the caller's own
# directory still means what it said.
args=(); for a in "$@"; do args+=("$(realpath -m "$a")"); done
cd "$(dirname "$0")"

make -s -C src/repro
src/repro/repro_driver "${args[@]}"
