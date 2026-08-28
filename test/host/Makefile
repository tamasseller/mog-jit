OUTPUT = jit-armv6m-host-tests

# Test code
SOURCES += test_armv6_encoding.cpp
SOURCES += test_window.cpp
SOURCES += test_assembler.cpp
SOURCES += test_runtime_arena.cpp
SOURCES += test_accstate.cpp
SOURCES += test_binops.cpp
SOURCES += test_abi_strategy.cpp
SOURCES += test_translate_proc.cpp
SOURCES += test_decode_encode.cpp
SOURCES += test_blocks.cpp
SOURCES += test_unaryops.cpp
SOURCES += test_proc_scan.cpp
SOURCES += test_entry_args.cpp
SOURCES += test_enter_program.cpp

# Test support
SOURCES += main.cpp
SOURCES += host_runtime_support.cpp
SOURCES += ../../compiler/src/encode_instr.cpp

# The compiler itself (armv6.h is header-only — test_armv6_encoding.cpp
# reaches it straight off INCLUDE_DIRS, no extra .cpp to compile)
SOURCES += ../../compiler/src/window.cpp
SOURCES += ../../compiler/src/accstate.cpp
SOURCES += ../../compiler/src/binops.cpp
SOURCES += ../../compiler/src/assembler.cpp
SOURCES += ../../compiler/src/shape.cpp
SOURCES += ../../compiler/src/abi_strategy.cpp
SOURCES += ../../compiler/src/decode_instr.cpp
SOURCES += ../../compiler/src/blocks.cpp
SOURCES += ../../compiler/src/unaryops.cpp
SOURCES += ../../compiler/src/proc_scan.cpp
SOURCES += ../../compiler/src/translate_proc.cpp

# The runtime's own entry layer. Compiles and links cleanly on the host —
# its only target-specific dependency is enterDispatch, which
# test_enter_program.cpp fakes so the descriptor it is handed can be
# inspected without booting an emulator.
SOURCES += ../../runtime/enter_program.cpp

# 1test library
SOURCES += ../../vendor/1test/TestRunner.cpp
SOURCES += ../../vendor/1test/PrintfOutput.cpp

INCLUDE_DIRS += ../../compiler/src   # jitc:: headers, armv6.h
INCLUDE_DIRS += ../../runtime         # runtime_internal.h / runtime_host.h
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
