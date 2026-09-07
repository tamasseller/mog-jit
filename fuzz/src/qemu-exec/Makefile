# The execution-oracle image: the real translator plus the real, unmodified
# runtime, linked for the same microbit model test/qemu targets. That is the
# whole point of the oracle — the code under test is byte-for-byte what that
# suite already validates on emulated hardware, so the flags come from the
# same support/qemu-image fragment rather than a copy kept in step by hand.
#
# `make DEBUG=1` builds the same runner with asserts live, into an ELF of its
# own: the shipped image is -DNDEBUG and has to stay byte-for-byte what that
# suite validates, so the assert sink cannot be the same binary. It gets its
# own linker script (shared with the coverage build), because neither assert
# strings nor trace-pc instrumentation fit the 16KB rom the batch window sits
# above — so those images put their batch somewhere else.
ROOT := ../../..

ifdef DEBUG
OUTPUT := exec_runner_dbg.elf
OBJDIR := .o/dbg
LDFLAGS += -T linker_big.ld
else ifdef COV
OUTPUT := exec_runner_cov.elf
OBJDIR := .o/cov
LDFLAGS += -T linker_big.ld
else
OUTPUT := exec_runner.elf
LDFLAGS += -T linker.ld
endif

include $(ROOT)/support/qemu-image/qemu-image.mk
include $(ROOT)/support/qemu-image/semihost.mk
include $(ROOT)/support/ext-rawmem/ext-rawmem-target.mk
include $(ROOT)/src/compiler/compiler-all.mk
include $(ROOT)/src/compiler/emit/ext-default.mk
include $(ROOT)/src/runtime/runtime-all.mk
include $(ROOT)/src/runtime/executor.mk
include $(ROOT)/src/runtime/dispatch.mk
include $(ROOT)/src/runtime/bytecode-default.mk

SOURCES += exec_runner.cpp

ifdef COV
SOURCES += cov_rt.cpp
endif

ifdef DEBUG
SOURCES += assert_report.cpp
endif

INCLUDE_DIRS += .

CXXFLAGS += -Os

# After qemu-image.mk, whose COMMONFLAGS carry the -DNDEBUG this undoes.
ifdef DEBUG
CXXFLAGS += -UNDEBUG -DPPL_BATCH_ADDR=0x8000u
endif

# `make COV=1` instruments every basic block (cov_rt.cpp). One call per block
# roughly doubles the image, so it gets a rom of its own and a batch window
# moved above it.
# `-g` costs the image nothing — DWARF is not a loaded section — and is what
# lets a campaign name the source line of a block it never reached.
ifdef COV
CXXFLAGS += -fsanitize-coverage=trace-pc -DPPL_COVERAGE -DPPL_BATCH_ADDR=0x8000u -g
endif

include $(ROOT)/vendor/ultimate-makefile/Makefile.ultimate
