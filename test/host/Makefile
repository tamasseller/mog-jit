OUTPUT = jit-armv6m-host-tests

# Test code
SOURCES += test_armv6_encoding.cpp
SOURCES += test_window.cpp
SOURCES += test_assembler.cpp
SOURCES += test_runtime_arena.cpp
SOURCES += test_accstate.cpp
SOURCES += test_abi_strategy.cpp
SOURCES += test_translate_proc.cpp
SOURCES += test_decode_encode.cpp
SOURCES += test_arithmetic.cpp
SOURCES += test_proc_scan.cpp
SOURCES += test_ext.cpp
SOURCES += test_entry_args.cpp
SOURCES += test_executor.cpp
SOURCES += test_bytecode_reader.cpp

# Test support
SOURCES += main.cpp
SOURCES += host_runtime_support.cpp
SOURCES += ext_stub.cpp
SOURCES += bc_stub.cpp
SOURCES += bc_buffered.cpp
include ../../support/bytecode/bytecode-tools.mk

# The translator, all three layers. ext-default.mk is left out: ext_stub.cpp
# above is this suite's extension set.
include ../../src/compiler/compiler-all.mk

# The runtime's own entry layer. Compiles and links cleanly on the host — its
# only target-specific dependency is enterDispatch, which test_executor.cpp
# fakes so the descriptor it is handed can be inspected without booting an
# emulator. dispatch.mk is target-only; host_runtime_support.cpp stands in.
include ../../src/runtime/runtime-all.mk
include ../../src/runtime/executor.mk

# 1test library
SOURCES += ../../vendor/1test/TestRunner.cpp
SOURCES += ../../vendor/1test/PrintfOutput.cpp

INCLUDE_DIRS += ../../vendor/1test

ASMFLAGS += -m32
LDFLAGS += -m32
CXXFLAGS += -m32
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
