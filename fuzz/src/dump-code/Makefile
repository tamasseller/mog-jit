# Translates one whole-program envelope on the host and writes each
# procedure's emitted Thumb out for objdump. Links the same rawmem extension
# the campaign's own images do: nearly every generated program uses it, and
# the weak default set refuses each one outright.
FUZZ := ../..

OUTPUT := dump_code

include ../host-driver.mk
include ../jit-host.mk
include $(FUZZ)/../support/ext-rawmem/ext-rawmem-host.mk
include $(FUZZ)/../support/dump-code/dump-code.mk

include $(FUZZ)/../vendor/ultimate-makefile/Makefile.ultimate
