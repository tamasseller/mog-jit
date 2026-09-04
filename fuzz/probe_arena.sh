#!/usr/bin/env bash
# Reports, per arena size, whether a seed actually reaches eviction -- see
# src/probe-arena/probe_arena.cpp's own header comment. Not part of a fuzzing run.
#
#   ./probe_arena.sh seeds/*
set -euo pipefail

# Resolve arguments before the cd, so a path relative to the caller's own
# directory still means what it said.
args=(); for a in "$@"; do args+=("$(realpath -m "$a")"); done
cd "$(dirname "$0")"

make -s -C src/probe-arena
src/probe-arena/probe_arena "${args[@]}"
