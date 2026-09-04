# One benchmark image, for one workload at one reference-kernel optimization
# level. Both are required; build.sh loops over the matrix.
#
# Exactly one thing varies across the levels: kernels_ref.cpp's own. The
# runtime, the translator and the runner stay at -Os in every image, so the
# JIT-side numbers must come out identical across all of them — which the
# driver checks, and which is a free consistency test on the measurement.

ifndef WORKLOAD
$(error WORKLOAD is required, e.g. WORKLOAD=pulse-trigger)
endif
ifndef LEVEL
$(error LEVEL is required, e.g. LEVEL=O2)
endif

BENCH := ../..
OUT_DIR ?= .

OUTPUT := $(OUT_DIR)/bench.$(WORKLOAD).$(LEVEL).elf
OBJDIR := .o/$(WORKLOAD)/$(LEVEL)

include $(BENCH)/../fuzz/qemu_exec/qemu-image.mk
include $(BENCH)/src/common/bench-common.mk
include $(BENCH)/../src/compiler/compiler-all.mk
include $(BENCH)/../src/runtime/runtime-all.mk
include $(BENCH)/../src/runtime/executor.mk
include $(BENCH)/../src/runtime/dispatch.mk
include $(BENCH)/../src/runtime/bytecode-default.mk

SOURCES += bench_stack.cpp
SOURCES += kernels_ref.cpp
SOURCES += bench_runner.cpp
SOURCES += $(BENCH)/generated/samples.cpp
SOURCES += $(BENCH)/generated/bench_data.cpp

# `#include "generated/bench_data.h"` resolves against the bench root.
INCLUDE_DIRS += $(BENCH)
INCLUDE_DIRS += .

CXXFLAGS += -Os -ffunction-sections

# build.sh labels each image itself.
OBJSIZE :=

# The reference kernels are the only thing whose level moves. -fstack-usage
# on them alone, so the .su the driver reads is attributable to them: the JIT
# side has no compile-time equivalent, its stack being a property of the
# program it runs rather than of any function GCC ever saw.
KERNELS_REF_CPP_FLAGS += -$(LEVEL) -fstack-usage

LDFLAGS += -T $(BENCH)/src/common/linker.ld

include $(BENCH)/../vendor/ultimate-makefile/Makefile.ultimate
