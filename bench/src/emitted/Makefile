# Host tool: translates one sample-stream program and writes out the emitted
# Thumb, for dump-emitted.sh to disassemble. fuzz/'s dump_code with this
# extension linked in place of the weak defaults.
#
# -m32: Runtime addresses its arena and every ProcSlot::bodyHandle as a bare
# uint32_t, so a host build has to be one where a real address fits.
BENCH := ../..

OUTPUT := dump_emitted

include $(BENCH)/src/common/bench-common.mk
include $(BENCH)/../src/compiler/compiler-all.mk
include $(BENCH)/../src/runtime/runtime-all.mk
include $(BENCH)/../src/runtime/bytecode-default.mk

SOURCES += $(BENCH)/../fuzz/dump_code.cpp
SOURCES += samples_stub.cpp

CXXFLAGS += -std=c++17 -O1 -g -m32
LDFLAGS += -m32

LD = $(CXX)

include $(BENCH)/../vendor/ultimate-makefile/Makefile.ultimate
