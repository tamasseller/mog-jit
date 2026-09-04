# Replays one saved input through the exact harness the fuzzer runs — same
# sanitizers, same two passes, one shot instead of a mutation loop.
FUZZ := ../..

OUTPUT := repro_driver

include ../host-driver.mk
include ../sanitizers.mk
include ../jit-host.mk
include $(FUZZ)/../support/ext-rawmem/ext-rawmem-host.mk
include ../driver/harness.mk

SOURCES += repro.cpp

# Switches off harness.cpp's own dumb-driver main, leaving repro.cpp's.
CXXFLAGS += -DPPL_FUZZ_LIBFUZZER_BUILD

include $(FUZZ)/../vendor/ultimate-makefile/Makefile.ultimate
