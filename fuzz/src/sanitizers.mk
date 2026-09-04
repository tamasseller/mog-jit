# ASan + UBSan, for the drivers whose whole job is to notice memory and
# undefined-behaviour bugs. -fno-sanitize=shift-base: the translator shifts by
# validator-approved amounts that UBSan cannot see are bounded.
CXXFLAGS := $(CXXFLAGS) -fsanitize=address,undefined -fno-sanitize-recover=all
CXXFLAGS := $(CXXFLAGS) -fno-sanitize=shift-base
LDFLAGS  := $(LDFLAGS) -fsanitize=address,undefined
