# The translator's headers, for a consumer that speaks its vocabulary without
# linking it — the runtime's dispatch layer, and the tools that only encode or
# decode bytecode. The one place the layer directories are named.
curdir := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

INCLUDE_DIRS := $(INCLUDE_DIRS) $(curdir)infra $(curdir)emit $(curdir)translate

undefine curdir
