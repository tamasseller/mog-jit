OUTPUT = jit-armv6m-host-tests

# Test code
SOURCES += test_armv6_encoding.cpp

# Test support
SOURCES += main.cpp

# 1test library
SOURCES += ../../vendor/1test/TestRunner.cpp
SOURCES += ../../vendor/1test/PrintfOutput.cpp

INCLUDE_DIRS += ../../src
INCLUDE_DIRS += ../../vendor/1test

CXXFLAGS += -std=c++17
CXXFLAGS += -O0                        # avoid the optimizer scrambling coverage data
CXXFLAGS += -g3
CXXFLAGS += -fno-inline                # ditto
CXXFLAGS += -fmax-errors=5
CXXFLAGS += --coverage
CXXFLAGS += -fno-exceptions
CXXFLAGS += -fdelete-null-pointer-checks

LIBS += gcov

LD = $(CXX)

include ../../vendor/ultimate-makefile/Makefile.ultimate
