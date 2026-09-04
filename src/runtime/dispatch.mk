# The real dispatch layer: hand-written ARMv6-M asm plus the helper vector both
# sides agree on. Target-only — a host build fakes runtimeBail and
# trampolineAddr instead. An addition to runtime-all.mk, and dispatch_abi.cpp
# is written in the translator's headers, so compiler-headers.mk is needed too.
curdir := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

SOURCES := $(SOURCES) $(curdir)runtime.S
SOURCES := $(SOURCES) $(curdir)dispatch_abi.cpp

undefine curdir
