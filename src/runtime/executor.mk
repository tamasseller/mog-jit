# Executor — memory configuration once, then one run() per program blob.
# An addition to runtime-all.mk.
curdir := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

SOURCES := $(SOURCES) $(curdir)executor.cpp

undefine curdir
