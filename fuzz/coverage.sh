#!/usr/bin/env bash
# Which translator branches a campaign never reaches.
#
# Runs the generator against the coverage build of the host sink instead of
# the ASan one — same programs, same two translation passes, no emulator —
# and reports line coverage over src/compiler and src/runtime.
#
#   ./coverage.sh [rounds] [extra driver flags...]
#
# The number to watch across campaigns is the uncovered set, not the
# percentage: a campaign that runs ten times as many programs over the same
# branches has bought nothing.
set -euo pipefail

cd "$(dirname "$0")"
rounds=${1:-3000}
shift || true

make -s -C src/driver COV=1
find src/driver/.o/cov -name '*.gcda' -delete 2>/dev/null || true

npx ts-node --transpile-only ts/driver.ts --rounds "$rounds" --batch 200 \
    --no-qemu --host-driver src/driver/fuzz_driver_cov "$@"

mkdir -p coverage
gcovr -r ../src --object-directory src/driver/.o/cov \
    --html-details -o coverage/coverage.html --print-summary
echo "coverage/coverage.html"
