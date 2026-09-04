# The fuzz driver. Plain build (default) is harness.cpp's own dumb mutation
# loop under ASan+UBSan, needing nothing but g++.
#
# `make AFL=1` is the coverage-guided build of the same entry point:
# afl-clang-fast++ (LLVM/SanitizerCoverage; the GCC_PLUGIN backend via
# afl-g++-fast fails to load here — prebuilt plugin vs. installed gcc ABI).
# ASan comes from AFL_USE_ASAN; UBSan is added by hand rather than through
# AFL_USE_UBSAN, which passes -fsanitize-undefined-trap-on-error and turns any
# UB into a bare SIGILL with no diagnostic. Debian's amd64 afl++ ships no
# afl-compiler-rt-32.o and afl-cc then refuses -m32 outright; README.md
# carries the AFL_PATH workaround.
FUZZ := ../..

ifdef AFL
OUTPUT := fuzz_driver_afl
OBJDIR := .o/afl
CXX := AFL_USE_ASAN=1 afl-clang-fast++
CXXFLAGS += -fsanitize=undefined -fno-sanitize-recover=all -fno-sanitize=shift-base
else
OUTPUT := fuzz_driver
OBJDIR := .o/plain
include ../sanitizers.mk
endif

include ../host-driver.mk
include ../jit-host.mk
include $(FUZZ)/../support/ext-rawmem/ext-rawmem-host.mk
include harness.mk

include $(FUZZ)/../vendor/ultimate-makefile/Makefile.ultimate
