# isa-core.md §11's empty extension seam, as weak definitions — safe to link
# beside a real extension set, which simply overrides them. An addition to
# emit-all.mk, never a substitute for it.
curdir := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

SOURCES := $(SOURCES) $(curdir)ext_default.cpp

undefine curdir
