# What every host driver here is built with. -m32: Runtime addresses its arena
# and every ProcSlot::bodyHandle as a bare uint32_t, so a host build has to be
# one where a real address fits.
CXXFLAGS := $(CXXFLAGS) -std=c++17 -O1 -g -m32
LDFLAGS  := $(LDFLAGS) -m32

LD = $(CXX)
