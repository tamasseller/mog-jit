# A minimal semihosting console: WRITE0 and EXIT. For an image that runs no
# tests — test/qemu has its own, which is a 1test TestOutput.
curdir := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

SOURCES := $(SOURCES) $(curdir)semihost.cpp

undefine curdir
