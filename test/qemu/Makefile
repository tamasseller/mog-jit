OUTPUT = jit-armv6m-qemu-tests.elf

# Vector table must be first on the link line — nothing else pins it to
# address 0 (see src/vectors.s).
SOURCES += ../../src/vectors.S

# Test code
SOURCES += smoke_test.cpp

# Test support
SOURCES += main.cpp
SOURCES += semihosting_output.cpp
SOURCES += cxx_stubs.cpp

# 1test library — TraceOutput/PrintfOutput are deliberately NOT compiled in:
# TraceOutput.cpp needs a Trace.h this project doesn't have (see
# docs/jit-armv6m.md's plan), and PrintfOutput needs a hosted <iostream>.
# Passing SemihostingOutput::instance explicitly to runAllTests() (main.cpp)
# means TestRunner.h's TraceOutput::instance default argument is never
# referenced, so this is not a link error.
SOURCES += ../../vendor/1test/TestRunner.cpp

INCLUDE_DIRS += ../../src
INCLUDE_DIRS += ../../vendor/1test

CC  = arm-none-eabi-gcc
CXX = arm-none-eabi-g++
LD  = arm-none-eabi-g++
AS  = arm-none-eabi-as
AR  = arm-none-eabi-ar
OBJCOPY = arm-none-eabi-objcopy
OBJSIZE = arm-none-eabi-size

ASMFLAGS += -mcpu=cortex-m0

CFLAGS   += -mcpu=cortex-m0 -mthumb -Os
CXXFLAGS += -mcpu=cortex-m0 -mthumb -Os
CXXFLAGS += -std=c++17
CXXFLAGS += -fno-exceptions
CXXFLAGS += -fno-rtti
CXXFLAGS += -fno-use-cxa-atexit

LDFLAGS += -mcpu=cortex-m0 -mthumb
LDFLAGS += -static -nostartfiles
LDFLAGS += -specs=nosys.specs   # stub out any syscall newlib internals want (_exit, _sbrk, ...);
                                # NOT -nostdlib — 1test's setjmp/longjmp and libgcc's software
                                # divide (no hardware DIV on Cortex-M0) both come from the
                                # toolchain runtime, not from vectors.S/our own code.
LDFLAGS += -T ../../src/linker.ld

include ../../vendor/ultimate-makefile/Makefile.ultimate

# Not using Makefile.ultimate's own `check` (host-execution + gcovr
# coverage assumptions don't fit a bare-metal image) — a bare pass/fail
# signal instead comes from qemu-system-arm's own exit code, which
# semihosting_output.cpp's SYS_EXIT_EXTENDED call sets directly (§ "Automated
# pass/fail from QEMU" in docs/jit-armv6m.md).
qemu-test: $(OUTPUT)
	qemu-system-arm -M lm3s811evb -m 8k -nographic \
	    -semihosting-config enable=on,target=native -kernel $(OUTPUT)

qemu-debug: $(OUTPUT)
	qemu-system-arm -M lm3s811evb -m 8k -nographic -s -S \
	    -semihosting-config enable=on,target=native -kernel $(OUTPUT)

.PHONY: qemu-test qemu-debug
