# The whole translator, all three layers. ext-default.mk stays opt-in.
curdir := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

include $(realpath $(curdir)translate/translate-all.mk $(curdir)compiler-headers.mk)

undefine curdir
