# The raw-memory extension: a real isa-core.md §11 extension set, used as the
# subject wherever the extension seam itself is under test. Target build —
# the helper is hand-written Thumb.
curdir := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

INCLUDE_DIRS := $(INCLUDE_DIRS) $(curdir)

SOURCES := $(SOURCES) $(curdir)ext_rawmem.cpp
SOURCES := $(SOURCES) $(curdir)ext_rawmem_helper.S

undefine curdir
