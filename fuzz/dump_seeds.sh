#!/usr/bin/env bash
# Stages ../support/bytecode/corpus_programs.h's bodies into seeds_raw/ -- see
# src/dump-seeds/dump_seeds.cpp's own header comment. Run this after changing
# corpus_programs.h, then ts/make_seeds.ts, which is what actually writes seeds/:
#
#   ./dump_seeds.sh
#   TS_NODE_PROJECT=tsconfig.json npx ts-node --transpile-only ts/make_seeds.ts
set -euo pipefail
cd "$(dirname "$0")"

make -s -C src/dump-seeds

mkdir -p seeds_raw
src/dump-seeds/dump_seeds
