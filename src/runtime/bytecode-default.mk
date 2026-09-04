# The stock BcReader backend. Exactly one implementation may be linked.
# An addition to runtime-headers.mk.
curdir := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

SOURCES := $(SOURCES) $(curdir)bytecode_default.cpp

undefine curdir
