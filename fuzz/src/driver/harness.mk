# The fuzz driver's body: run every program in a batch through the real
# translateProc() pipeline. Its own main() is compiled in unless
# PPL_FUZZ_LIBFUZZER_BUILD is defined, which is how repro/ reuses it under a
# main of its own.
curdir := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

INCLUDE_DIRS := $(INCLUDE_DIRS) $(curdir)

SOURCES := $(SOURCES) $(curdir)harness.cpp

undefine curdir
