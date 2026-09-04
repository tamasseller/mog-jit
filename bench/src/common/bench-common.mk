# What every bench image shares: the sample-stream extension both halves of
# the benchmark are written against, and the measurement-region markers the
# QEMU plugin matches on.
curdir := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

INCLUDE_DIRS := $(INCLUDE_DIRS) $(curdir)

SOURCES := $(SOURCES) $(curdir)ext_sampstream.cpp

undefine curdir
