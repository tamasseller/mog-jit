# The QEMU TCG plugin that counts executed instructions per marked region.
# A host shared object, not part of any image. qemu-plugin.h is vendored
# rather than taken from a development package: Debian ships qemu-system-arm
# without it, and pinning the header is what makes the API-version guard mean
# something.
OUTPUT := bench_plugin.so

SOURCES += bench_plugin.c

INCLUDE_DIRS += .

CFLAGS += -O2 -g -Wall -Wextra -fPIC
LDFLAGS += -shared

LD = $(CC)

include ../../../vendor/ultimate-makefile/Makefile.ultimate
