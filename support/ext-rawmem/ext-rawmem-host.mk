# The same extension for a host driver, where the Thumb helper cannot run and
# a C++ stand-in takes its place.
curdir := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

INCLUDE_DIRS := $(INCLUDE_DIRS) $(curdir)

SOURCES := $(SOURCES) $(curdir)ext_rawmem.cpp
SOURCES := $(SOURCES) $(curdir)rawmem_helper_host.cpp

undefine curdir
