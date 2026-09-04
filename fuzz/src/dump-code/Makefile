# Translates one whole-program envelope on the host and writes each
# procedure's emitted Thumb out for objdump. The weak default extension set —
# bench/src/emitted builds the same tool with its own.
FUZZ := ../..

OUTPUT := dump_code

include ../host-driver.mk
include ../jit-host.mk
include $(FUZZ)/../support/dump-code/dump-code.mk

include $(FUZZ)/../vendor/ultimate-makefile/Makefile.ultimate
