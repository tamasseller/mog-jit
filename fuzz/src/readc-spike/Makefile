# See spike.cpp — a one-question probe, not part of any campaign.
ROOT := ../../..

OUTPUT := spike.elf

include $(ROOT)/support/qemu-image/qemu-image.mk
include $(ROOT)/support/qemu-image/semihost.mk

SOURCES += spike.cpp
INCLUDE_DIRS += .
CXXFLAGS += -Os
LDFLAGS += -T linker.ld

include $(ROOT)/vendor/ultimate-makefile/Makefile.ultimate
