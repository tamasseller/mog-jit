# The plugin's own honesty check: an image with a region around a loop of
# exactly known length, and one around nothing. selftest.sh reads the marker
# addresses out of this ELF and differences the two counts.
BENCH := ../..
QEMU_EXEC := $(BENCH)/../fuzz/qemu_exec

OUTPUT := selftest.elf

include $(QEMU_EXEC)/qemu-image.mk

SOURCES += selftest.cpp

INCLUDE_DIRS += $(BENCH)/src/common

CXXFLAGS += -Os

LDFLAGS += -T $(QEMU_EXEC)/linker.ld

include $(BENCH)/../vendor/ultimate-makefile/Makefile.ultimate
