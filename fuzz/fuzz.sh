#!/usr/bin/env bash
# The fuzzer. One entry point: it builds what it needs, checks its own
# oracles, measures what the seeds reach, then runs the campaign with the
# lanes interleaved and reports where programs died.
#
#   ./fuzz/fuzz.sh                  # until killed (Ctrl-C reports and exits)
#   ./fuzz/fuzz.sh --for 2h         # or 90m, 300s
#   ./fuzz/fuzz.sh --rounds 20000
#   ./fuzz/fuzz.sh --stop-on-finding
#   ./fuzz/fuzz.sh --calibrate-only # the self-checks alone
#   ./fuzz/fuzz.sh --seed 4242      # a different sequence, same everything
#
# Exits non-zero on a finding or a failed self-check.
set -euo pipefail
cd "$(dirname "$0")"
exec npx ts-node --transpile-only ts/fuzz.ts "$@"
