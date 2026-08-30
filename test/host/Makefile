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
SOURCES += test_enter_program.cpp

# Test support
SOURCES += main.cpp
SOURCES += host_runtime_support.cpp
SOURCES += ext_stub.cpp
SOURCES += ../encode_instr.cpp

# The compiler itself (armv6.h is header-only — test_armv6_encoding.cpp
# reaches it straight off INCLUDE_DIRS, no extra .cpp to compile)
SOURCES += ../../src/compiler/window.cpp
SOURCES += ../../src/compiler/ext.cpp
SOURCES += ../../src/compiler/accstate.cpp
SOURCES += ../../src/compiler/assembler.cpp
SOURCES += ../../src/compiler/arithmetic.cpp
SOURCES += ../../src/compiler/shape.cpp
SOURCES += ../../src/compiler/abi_strategy.cpp
SOURCES += ../../src/compiler/decode_instr.cpp
SOURCES += ../../src/compiler/proc_scan.cpp
SOURCES += ../../src/compiler/translate_proc.cpp
SOURCES += ../../src/compiler/translate_data_flow.cpp
SOURCES += ../../src/compiler/translate_control_flow.cpp

# The runtime's own entry layer. Compiles and links cleanly on the host —
# its only target-specific dependency is enterDispatch, which
# test_enter_program.cpp fakes so the descriptor it is handed can be
# inspected without booting an emulator.
SOURCES += ../../src/runtime/enter_program.cpp
SOURCES += ../../src/runtime/runtime.cpp

# 1test library
SOURCES += ../../vendor/1test/TestRunner.cpp
SOURCES += ../../vendor/1test/PrintfOutput.cpp

INCLUDE_DIRS += ..               
INCLUDE_DIRS += ../../src/runtime
INCLUDE_DIRS += ../../src/compiler
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
