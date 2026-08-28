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
SOURCES += ../../runtime/enter_program.cpp
SOURCES += ../../runtime/dispatch_abi.cpp
SOURCES += ../../runtime/compile_proc.cpp

# The compiler itself
SOURCES += ../../compiler/src/window.cpp
SOURCES += ../../compiler/src/ext.cpp
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
SOURCES += stack_paint.cpp
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
INCLUDE_DIRS += ..                      # corpus_programs.h, shared with test/tools/dump_corpus.cpp
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

# Emits a .su file per translation unit (one line per function: frame
# bytes, "static"/"dynamic"/"dynamic,bounded") next to its .o, under
# these exact -Os/-mcpu flags — optimization/inlining-dependent, so this
# must be measured from a real build with real flags, never estimated.
# check_stack_usage.py verifies these against the hand-derived constants
# in runtime/dispatch_abi.h and compiler/src/translate_proc.cpp/
# proc_scan.cpp (docs/design.md G2/G3/G5) — see `stack-usage-check` below.
CXXFLAGS += -fstack-usage

# Cheap backstop for the one thing check_stack_usage.py's static-byte-count
# comparison can't catch on its own: if a future change ever introduces an
# unbounded alloca/VLA into a tracked function, -fstack-usage reports it as
# "dynamic" with an unreliable byte count, silently voiding the whole
# per-level budget this build's stack-safety strategy depends on.
#
# Not on enter_program.cpp, and it cannot be: that file has one deliberate,
# load-bearing VLA (the Runtime storage, sized to the program's own
# procCount and already the first term of requiredStackBytes), and this
# flag's "stack usage might be unbounded" diagnostic fires regardless of
# the byte limit given, with no way to exempt a single known allocation.
# check_stack_usage.py's own whole-image scan covers that file instead —
# and covers every other translation unit too, including the ones this
# per-file flag was never attached to.
# -Werror=stack-usage=N fails the build immediately in that case instead —
# N is deliberately generous (double the largest tracked frame today) so
# this never fires on an ordinary size change, only on genuinely unbounded
# stack usage. Per-file, via ultimate-makefile's own flag-hook convention
# (<FILE_FLAGS>, e.g. TRANSLATE_PROC_CPP_FLAGS), on every file with a
# function this build tracks.
COMPILE_PROC_CPP_FLAGS += -Werror=stack-usage=512
TRANSLATE_PROC_CPP_FLAGS += -Werror=stack-usage=512
PROC_SCAN_CPP_FLAGS += -Werror=stack-usage=512
ABI_STRATEGY_CPP_FLAGS += -Werror=stack-usage=512
ASSEMBLER_CPP_FLAGS += -Werror=stack-usage=512

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

# Makefile.ultimate generates header dependencies for .c/.cpp (-MMD) but its
# %.S.o rule has neither DEPFLAGS nor a .d prerequisite, so an assembly file
# never notices a header change. runtime.S really does #include
# runtime_host.h (see the AS comment above), and the macros it takes from
# there — RUNTIME_DISPATCH_TABLE_OFFSET, DISPATCH_SENTINEL_OFFSET, LANDING_*,
# ENTRY_ARGS_*_OFFSET — are exactly the ones the C++ side also compiles
# against. A stale runtime.S.o therefore produces a binary whose two halves
# disagree about a struct offset, which does not fail to build and does not
# assert: it dispatches to a wrong address and hangs. runtime_internal.h's
# own static_assert cannot catch it, since it only checks the C++ side.
$(OBJDIR)/$(abspath ../../runtime/runtime.S).o: $(abspath ../../runtime/runtime_host.h)

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

# Depends on $(OUTPUT) (not just the .o rules directly) so every tracked
# .su file is guaranteed fresh from a real build with real flags before
# comparison — see check_stack_usage.py's own header for what this
# verifies and why (docs/design.md G2/G3/G5).
stack-usage-check: $(OUTPUT)
	python3 check_stack_usage.py $(OBJDIR)

.PHONY: qemu-test qemu-debug stack-usage-check
