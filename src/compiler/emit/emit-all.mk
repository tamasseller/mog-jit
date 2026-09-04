# The emitters and the state they track.
curdir := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

SOURCES := $(SOURCES) $(curdir)abi_strategy.cpp
SOURCES := $(SOURCES) $(curdir)accstate.cpp
SOURCES := $(SOURCES) $(curdir)arithmetic.cpp
SOURCES := $(SOURCES) $(curdir)ext.cpp
SOURCES := $(SOURCES) $(curdir)shape.cpp
SOURCES := $(SOURCES) $(curdir)window.cpp

include $(realpath $(curdir)../infra/infra-all.mk)

undefine curdir
