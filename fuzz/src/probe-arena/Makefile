# Reports, per arena size, whether a seed actually reaches eviction. Not part
# of a fuzzing run — no sanitizers, since nothing here is looking for a crash.
FUZZ := ../..

OUTPUT := probe_arena

include ../host-driver.mk
include ../jit-host.mk

SOURCES += probe_arena.cpp

include $(FUZZ)/../vendor/ultimate-makefile/Makefile.ultimate
