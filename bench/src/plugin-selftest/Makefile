# The plugin's own honesty check: an image with a region around a loop of
# exactly known length, and one around nothing. selftest.sh reads the marker
# addresses out of this ELF and differences the two counts.
BENCH := ../..

OUTPUT := selftest.elf

include $(BENCH)/../support/qemu-image/qemu-image.mk
include $(BENCH)/../support/qemu-image/semihost.mk

SOURCES += selftest.cpp

INCLUDE_DIRS += $(BENCH)/src/common

CXXFLAGS += -Os

LDFLAGS += -T $(BENCH)/src/common/linker.ld

include $(BENCH)/../vendor/ultimate-makefile/Makefile.ultimate
