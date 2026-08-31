#!/usr/bin/env bash
# Build the counting plugin. The header is vendored rather than taken from a
# qemu development package: Debian ships qemu-system-arm without it, and
# pinning the header is what makes the API-version guard meaningful.
set -euo pipefail
cd "$(dirname "$0")"

gcc -O2 -g -Wall -Wextra -fPIC -shared -o bench_plugin.so bench_plugin.c

echo "built $(pwd)/bench_plugin.so"
