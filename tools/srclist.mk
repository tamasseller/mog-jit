# Resolves a set of src/**/*.mk fragments into a flat source list or -I flags,
# for the hand-rolled build scripts under fuzz/ and bench/ — those are plain
# g++ invocations, not ultimate-makefile projects, but they need the same
# lists. Driven by tools/srclist.sh.
include $(FRAGMENTS)

sources:
	@echo $(SOURCES)

includes:
	@echo $(addprefix -I,$(INCLUDE_DIRS))

.PHONY: sources includes
