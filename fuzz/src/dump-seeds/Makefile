# Re-encodes the shared corpus's single-procedure bodies into seeds_raw/.
# Encoder and headers only: nothing here translates, so no runtime is linked
# and the host word size does not matter.
FUZZ := ../..

OUTPUT := dump_seeds

include $(FUZZ)/../src/compiler/compiler-headers.mk
include $(FUZZ)/../src/runtime/runtime-headers.mk
include $(FUZZ)/../src/runtime/bytecode-default.mk
include $(FUZZ)/../support/bytecode/bytecode-tools.mk

SOURCES += dump_seeds.cpp

CXXFLAGS += -std=c++17 -O1

LD = $(CXX)

include $(FUZZ)/../vendor/ultimate-makefile/Makefile.ultimate
