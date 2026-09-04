# Building bytecode by hand: the encoder the tests and the seed dumper write
# programs with, the shared corpus of them, and the whole-program envelope
# format the host drivers read.
curdir := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

INCLUDE_DIRS := $(INCLUDE_DIRS) $(curdir)

SOURCES := $(SOURCES) $(curdir)encode_instr.cpp

undefine curdir
