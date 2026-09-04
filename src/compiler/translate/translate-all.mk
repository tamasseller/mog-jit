# The bytecode-driven logic on top of the emitters.
curdir := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

SOURCES := $(SOURCES) $(curdir)translate_control_flow.cpp
SOURCES := $(SOURCES) $(curdir)translate_data_flow.cpp
SOURCES := $(SOURCES) $(curdir)translate_proc.cpp

include $(realpath $(curdir)../emit/emit-all.mk)

undefine curdir
