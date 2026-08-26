# Builds and runs the hand-transcribed fixtures (fixtures.cpp), the
# scenario TESTs (main.cpp), and a trivial harness-sanity TEST
# (smoke_test.cpp) against the real translator (jit-armv6m/compiler/src)
# and the real, unmodified dispatch/eviction runtime (jit-armv6m/runtime).
# No program.cpp code-generation step, unlike the now-retired TS
# prototype's own Makefile — everything here is a checked-in, hand-written
# source file. Test cases are 1test TESTs (the same framework test/host
# runs), reported over semihosting (semihosting_output.cpp) rather than
# test/host's PrintfOutput — this target has no <iostream>.

OUTPUT = run.elf

# Vector table must be first on the link line — nothing else pins it to
# address 0 (vectors.S).
SOURCES += vectors.S

SOURCES += ../../runtime/runtime.S
SOURCES += ../../runtime/runtime_host.cpp
SOURCES += ../../runtime/compile_proc_real.cpp

# The compiler itself
SOURCES += ../../compiler/src/window.cpp
SOURCES += ../../compiler/src/accstate.cpp
SOURCES += ../../compiler/src/binops.cpp
SOURCES += ../../compiler/src/assembler.cpp
SOURCES += ../../compiler/src/shape.cpp
SOURCES += ../../compiler/src/abi_strategy.cpp
SOURCES += ../../compiler/src/decode_instr.cpp
SOURCES += ../../compiler/src/blocks.cpp
SOURCES += ../../compiler/src/unaryops.cpp
SOURCES += ../../compiler/src/proc_scan.cpp
SOURCES += ../../compiler/src/translate_proc.cpp

# Test code (encode_instr.cpp is test-only here — the real compiler only
# ever decodes, fixtures.cpp is what needs to produce wire bytes)
SOURCES += ../../compiler/src/encode_instr.cpp
SOURCES += fixtures.cpp
SOURCES += smoke_test.cpp
SOURCES += semihosting_output.cpp
SOURCES += cxx_stubs.cpp
SOURCES += main.cpp

# 1test library — TestRunner.cpp only; PrintfOutput.cpp needs <iostream>,
# unavailable on this bare-metal target (semihosting_output.cpp is this
# build's own TestOutput sink instead).
SOURCES += ../../vendor/1test/TestRunner.cpp

INCLUDE_DIRS += ../../compiler/src   # jitc:: headers, armv6.h
INCLUDE_DIRS += ../../runtime         # runtime_host.h / runtime_internal.h
INCLUDE_DIRS += .                      # fixtures.h / semihosting_output.h
INCLUDE_DIRS += ../../vendor/1test      # Test.h

# arm-none-eabi-g++ for AS too (not arm-none-eabi-as): runtime.S has a real
# `#include "runtime_host.h"` that needs preprocessing, which the g++
# driver runs automatically for a capital-.S source, same as it already
# does for CXX's own .cpp files.
CC  = arm-none-eabi-gcc
CXX = arm-none-eabi-g++
AS  = arm-none-eabi-g++
LD  = arm-none-eabi-g++
OBJCOPY = arm-none-eabi-objcopy
OBJSIZE = arm-none-eabi-size

ASMFLAGS += -mcpu=cortex-m0 -mthumb

# -DNDEBUG: this target is bare-metal (-nostartfiles -specs=nosys.specs) —
# a firing assert() would need newlib's full fprintf-based failure path
# (and the sbrk/heap plumbing behind it), which alone blows any realistic
# rom budget many times over. test/host already exercises every one of
# this code's assertions with real libc behind it; this build only needs
# the invariants compiled *out*, not checked.
CXXFLAGS += -mcpu=cortex-m0 -mthumb -Os -DNDEBUG
CXXFLAGS += -std=gnu++17 -fno-exceptions -fno-rtti

# runtime.S's own hand-written asm (and every `register ... asm("r8"/"r9"/
# "r10"/"r11")` C++ site built alongside it — dispatch base, the Runtime
# pointer, the helper-vector base, the LRU tick) all assume the compiler's
# own register allocator never touches r8-r11 for anything else. That
# assumption was previously undocumented in the build itself — asserted
# only in comments (runtime.S, docs/design.md) — so nothing actually
# enforced it.
CXXFLAGS += -ffixed-r8 -ffixed-r9 -ffixed-r10 -ffixed-r11

# 1test's TestHandle/TestOutput both have virtual destructors, so g++
# registers their static instances for destruction at exit — with the
# default __cxa_atexit-based scheme that means a real __dso_handle symbol,
# which this bare-metal link has no reason to define. -fno-use-cxa-atexit
# routes that registration through plain atexit() instead, which newlib
# services from a small static slot table (no malloc) for the handful of
# statics this image ever registers. cxx_stubs.cpp handles the other half
# of the same problem — the deleting-destructor's own operator delete
# reference, never actually called either.
CXXFLAGS += -fno-use-cxa-atexit

LDFLAGS += -mcpu=cortex-m0 -mthumb
LDFLAGS += -static -nostartfiles
LDFLAGS += -specs=nosys.specs
# A real translator + the real dispatch/eviction runtime, linked together,
# needs more than a minimal smoke test's own rom/ram — see linker.ld's own
# header for the exact history. 32KB rom is still half of the lm3s811evb's
# real 64KB flash.
LDFLAGS += -T linker.ld

include ../../vendor/ultimate-makefile/Makefile.ultimate

# Not using Makefile.ultimate's own `check` (host-execution + gcovr
# coverage assumptions don't fit a bare-metal image) — main.cpp's own
# semihostingExit(0/1) is the pass/fail signal, surfaced through
# qemu-system-arm's exit code.
qemu-test: $(OUTPUT)
	qemu-system-arm -M lm3s811evb -m 8k -nographic -monitor none \
	    -semihosting-config enable=on,target=native -kernel $(OUTPUT)

qemu-debug: $(OUTPUT)
	qemu-system-arm -M lm3s811evb -m 8k -nographic -monitor none -s -S \
	    -semihosting-config enable=on,target=native -kernel $(OUTPUT)

.PHONY: qemu-test qemu-debug
