#!/usr/bin/env bash
# Replays one saved input through the exact harness the fuzzer runs.
#
#   ./repro.sh last_input.bin
set -euo pipefail

# Resolve arguments before the cd, so a path relative to the caller's own
# directory still means what it said.
args=(); for a in "$@"; do args+=("$(realpath -m "$a")"); done
cd "$(dirname "$0")"

make -s -C src/repro
src/repro/repro_driver "${args[@]}"
