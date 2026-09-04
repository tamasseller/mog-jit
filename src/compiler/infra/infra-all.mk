# The translator's bottom layer: the Thumb assembler, the bytecode decoder and
# the body scanner, plus the headers everything above them is written in.
curdir := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

SOURCES := $(SOURCES) $(curdir)assembler.cpp
SOURCES := $(SOURCES) $(curdir)decode_instr.cpp
SOURCES := $(SOURCES) $(curdir)proc_scan.cpp

undefine curdir
