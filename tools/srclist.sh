#!/usr/bin/env bash
# Prints what a set of src/**/*.mk fragments contributes: the source list, or
# with --includes the -I flags. Lets a build script name the layers it wants
# instead of tracking their files.
#
#   srclist.sh ../src/compiler/sources.mk ../src/runtime/sources.mk
#   srclist.sh --includes ../src/compiler/include.mk
set -euo pipefail

goal=sources
if [ "${1:-}" = "--includes" ]; then goal=includes; shift; fi

make -s -f "$(dirname "${BASH_SOURCE[0]}")/srclist.mk" FRAGMENTS="$*" "$goal"
