# Translates one whole-program envelope on the host and writes each
# procedure's emitted Thumb to a raw .bin, for objdump to disassemble. The
# extension set is whatever the consumer links: fuzz/ takes the weak defaults,
# bench/ its own.
curdir := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

SOURCES := $(SOURCES) $(curdir)dump_code.cpp

undefine curdir
