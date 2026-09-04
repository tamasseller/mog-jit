# The translator and enough runtime to drive it on the host. Which extension
# set gets linked is each driver's own choice — the weak defaults stand
# unless it says otherwise.
curdir := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
root := $(realpath $(curdir)../..)

include $(realpath $(root)/src/compiler/compiler-all.mk)
include $(realpath $(root)/src/compiler/emit/ext-default.mk)
include $(realpath $(root)/src/runtime/runtime-all.mk)
include $(realpath $(root)/src/runtime/bytecode-default.mk)
include $(realpath $(root)/support/bytecode/bytecode-tools.mk)

undefine curdir
undefine root
