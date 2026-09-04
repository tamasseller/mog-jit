# Bare-metal support for a microbit image under QEMU: the vector table, the
# semihosting console and the C++ runtime stubs, plus the toolchain and flags
# every such image is built with. Each image brings its own linker script.
#
# Include this FIRST — vectors.S has to be the first object on the link line,
# nothing else pins the vector table to address 0.
#
# Flags are kept in step with test/qemu/Makefile's. The -ffixed-r8..r11
# reservations and -DNDEBUG are the two that matter: runtime.S and every
# `register ... asm("r9")` site depend on the allocator never touching those,
# and a firing assert() would pull in newlib's fprintf path.
curdir := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

INCLUDE_DIRS := $(INCLUDE_DIRS) $(curdir)

SOURCES := $(SOURCES) $(curdir)vectors.S
SOURCES := $(SOURCES) $(curdir)semihost.cpp
SOURCES := $(SOURCES) $(curdir)cxx_stubs.cpp

CC      := arm-none-eabi-gcc
CXX     := arm-none-eabi-g++
AS      := arm-none-eabi-g++
LD      := arm-none-eabi-g++
OBJCOPY := arm-none-eabi-objcopy
OBJSIZE := arm-none-eabi-size

COMMONFLAGS := -mcpu=cortex-m0 -mthumb -DNDEBUG
COMMONFLAGS := $(COMMONFLAGS) -ffixed-r8 -ffixed-r9 -ffixed-r10 -ffixed-r11
COMMONFLAGS := $(COMMONFLAGS) -fno-use-cxa-atexit

ASMFLAGS := $(ASMFLAGS) -mcpu=cortex-m0 -mthumb
CXXFLAGS := $(CXXFLAGS) $(COMMONFLAGS) -std=gnu++17 -fno-exceptions -fno-rtti
LDFLAGS  := $(LDFLAGS)  -mcpu=cortex-m0 -mthumb -static -nostartfiles -specs=nosys.specs

undefine curdir
