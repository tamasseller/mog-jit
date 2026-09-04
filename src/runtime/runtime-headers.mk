# Runtime headers — Runtime itself is header-only and portable, so a consumer
# may need these without linking any runtime object.
curdir := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

INCLUDE_DIRS := $(INCLUDE_DIRS) $(curdir)

undefine curdir
