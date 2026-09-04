# Runtime state every image that actually translates needs: the code arena,
# the dispatch table and eviction.
curdir := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

SOURCES := $(SOURCES) $(curdir)runtime.cpp

include $(realpath $(curdir)runtime-headers.mk)

undefine curdir
