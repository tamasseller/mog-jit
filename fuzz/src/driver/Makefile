# The crash sink: harness.cpp's two translation passes over a batch of
# programs the TS driver has already lowered and validated. Default build is
# ASan+UBSan with asserts live, needing nothing but g++.
#
# `make COV=1` builds the same entry point for line coverage instead —
# unoptimised, uninlined and unsanitised, since the question that build
# answers is which translator branches a campaign never reaches. The two are
# separate outputs, so a campaign can run either without a rebuild.
FUZZ := ../..

ifdef COV
OUTPUT := fuzz_driver_cov
OBJDIR := .o/cov
CXXFLAGS += -O0 -fno-inline --coverage
LIBS += gcov
else
OUTPUT := fuzz_driver
OBJDIR := .o/plain
include ../sanitizers.mk
endif

include ../host-driver.mk
include ../jit-host.mk
include $(FUZZ)/../support/ext-rawmem/ext-rawmem-host.mk
include harness.mk

include $(FUZZ)/../vendor/ultimate-makefile/Makefile.ultimate
