#!/usr/bin/env bash
# Regenerates the diverse-shape seeds/ entries from dump_seeds.cpp -- see
# its own header comment. Not part of the fuzzing run itself (build.sh/
# build_afl.sh); run this manually after changing
# ../test/corpus_programs.h to refresh the corresponding seed files.
set -euo pipefail
cd "$(dirname "$0")"

g++ -std=c++17 -O1 \
    -I ../compiler/src -I ../test \
    dump_seeds.cpp \
    ../compiler/src/encode_instr.cpp \
    -o dump_seeds

./dump_seeds
