# The execution-oracle image: the real translator plus the real, unmodified
# runtime, linked for the same microbit model test/qemu targets. That is the
# whole point of the oracle — the code under test is byte-for-byte what that
# suite already validates on emulated hardware, so the flags come from the
# same support/qemu-image fragment rather than a copy kept in step by hand.
ROOT := ../../..

OUTPUT := exec_runner.elf

include $(ROOT)/support/qemu-image/qemu-image.mk
include $(ROOT)/support/qemu-image/semihost.mk
include $(ROOT)/support/ext-rawmem/ext-rawmem-target.mk
include $(ROOT)/src/compiler/compiler-all.mk
include $(ROOT)/src/compiler/emit/ext-default.mk
include $(ROOT)/src/runtime/runtime-all.mk
include $(ROOT)/src/runtime/executor.mk
include $(ROOT)/src/runtime/dispatch.mk
include $(ROOT)/src/runtime/bytecode-default.mk

SOURCES += exec_runner.cpp

INCLUDE_DIRS += .

CXXFLAGS += -Os

LDFLAGS += -T linker.ld

include $(ROOT)/vendor/ultimate-makefile/Makefile.ultimate
