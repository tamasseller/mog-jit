# Builds and runs all 7 hand-transcribed fixtures (fixtures.cpp) against
# the real translator (jit-armv6m/compiler/src) and the real, unmodified
# dispatch/eviction runtime (jit-armv6m/runtime). No program.cpp
# code-generation step, unlike the now-retired TS prototype's own Makefile —
# everything here is a checked-in, hand-written source file.

OUTPUT = run.elf

# Vector table must be first on the link line — nothing else pins it to
# address 0 (jit-armv6m/src/vectors.S).
SOURCES += ../../../src/vectors.S

SOURCES += ../../../runtime/semihosting.cpp
SOURCES += ../../../runtime/runtime.S
SOURCES += ../../../runtime/runtime_host.cpp
SOURCES += ../../qemu/compile_proc_real.cpp

# The compiler itself
SOURCES += ../../src/window.cpp
SOURCES += ../../src/accstate.cpp
SOURCES += ../../src/binops.cpp
SOURCES += ../../src/imm_synth.cpp
SOURCES += ../../src/shape.cpp
SOURCES += ../../src/abi_strategy.cpp
SOURCES += ../../src/decode_instr.cpp
SOURCES += ../../src/blocks.cpp
SOURCES += ../../src/unaryops.cpp
SOURCES += ../../src/translate_proc.cpp

# Test code (encode_instr.cpp is test-only here — the real compiler only
# ever decodes, fixtures.cpp is what needs to produce wire bytes)
SOURCES += ../../src/encode_instr.cpp
SOURCES += fixtures.cpp
SOURCES += main.cpp

INCLUDE_DIRS += ../../../src            # armv6.h
INCLUDE_DIRS += ../../../runtime         # runtime_host.h / runtime_internal.h
INCLUDE_DIRS += ../../src                 # jitc:: headers
INCLUDE_DIRS += .                          # fixtures.h

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
# rom budget many times over. The host test suite
# (jit-armv6m/compiler/test/host) already exercises every one of this
# code's assertions with real libc behind it; this build only needs the
# invariants compiled *out*, not checked.
CXXFLAGS += -mcpu=cortex-m0 -mthumb -Os -DNDEBUG
CXXFLAGS += -std=gnu++17 -fno-exceptions -fno-rtti

LDFLAGS += -mcpu=cortex-m0 -mthumb
LDFLAGS += -static -nostartfiles
LDFLAGS += -specs=nosys.specs
# A real translator + the real dispatch/eviction runtime, linked together,
# no longer fits jit-armv6m/src/linker.ld's own 4KB rom (sized back when
# compileProc was a bare memcpy) — linker.ld's own header already flags
# that region as a deliberately small slice of the lm3s811evb's real 64KB
# flash, not the actual ceiling. This test's own linker.ld bumps just that
# region (still a fraction of the real chip); it does not touch the
# shared jit-armv6m/src/linker.ld other builds rely on.
LDFLAGS += -T linker.ld

include ../../../vendor/ultimate-makefile/Makefile.ultimate

# Not using Makefile.ultimate's own `check` (host-execution + gcovr
# coverage assumptions don't fit a bare-metal image) — main.cpp's own
# semihostingExit(0/1) is the pass/fail signal, surfaced through
# qemu-system-arm's exit code.
qemu-test: $(OUTPUT)
	qemu-system-arm -M lm3s811evb -m 8k -nographic -monitor none \
	    -semihosting-config enable=on,target=native -kernel $(OUTPUT)

.PHONY: qemu-test
