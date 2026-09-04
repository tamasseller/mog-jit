# The sample-stream extension's acceptance-gate image: the real translator and
# the real, unmodified runtime, running a generated program whose every
# touched value check.sh compares against @ppl/machine's VM.
BENCH := ../..
OUT_DIR ?= .

OUTPUT := $(OUT_DIR)/check.elf

include $(BENCH)/../fuzz/qemu_exec/qemu-image.mk
include $(BENCH)/src/common/bench-common.mk
include $(BENCH)/../src/compiler/compiler-all.mk
include $(BENCH)/../src/runtime/runtime-all.mk
include $(BENCH)/../src/runtime/executor.mk
include $(BENCH)/../src/runtime/dispatch.mk
include $(BENCH)/../src/runtime/bytecode-default.mk

SOURCES += check_runner.cpp
SOURCES += $(BENCH)/generated/check_samples.cpp
SOURCES += $(BENCH)/generated/check_data.cpp

# `#include "generated/check_data.h"` resolves against the bench root.
INCLUDE_DIRS += $(BENCH)

CXXFLAGS += -Os

LDFLAGS += -T $(BENCH)/src/common/linker.ld

include $(BENCH)/../vendor/ultimate-makefile/Makefile.ultimate
