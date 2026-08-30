OUTPUT = run.elf

SOURCES += vectors.S

SOURCES += ../../src/runtime/runtime.S
SOURCES += ../../src/runtime/runtime.cpp
SOURCES += ../../src/runtime/enter_program.cpp
SOURCES += ../../src/runtime/dispatch_abi.cpp

# The compiler itself
SOURCES += ../../src/compiler/window.cpp
SOURCES += ../../src/compiler/ext.cpp
SOURCES += ../../src/compiler/ext_default.cpp
SOURCES += ../../src/compiler/accstate.cpp
SOURCES += ../../src/compiler/arithmetic.cpp
SOURCES += ../../src/compiler/assembler.cpp
SOURCES += ../../src/compiler/shape.cpp
SOURCES += ../../src/compiler/abi_strategy.cpp
SOURCES += ../../src/compiler/decode_instr.cpp
SOURCES += ../../src/compiler/proc_scan.cpp
SOURCES += ../../src/compiler/translate_proc.cpp
SOURCES += ../../src/compiler/translate_data_flow.cpp
SOURCES += ../../src/compiler/translate_control_flow.cpp

SOURCES += ../encode_instr.cpp
SOURCES += fixtures.cpp
SOURCES += smoke_test.cpp
SOURCES += stack_paint.cpp
SOURCES += semihosting_output.cpp
SOURCES += cxx_stubs.cpp
SOURCES += main.cpp

SOURCES += ../../vendor/1test/TestRunner.cpp

INCLUDE_DIRS += .
INCLUDE_DIRS += ..
INCLUDE_DIRS += ../../src/runtime
INCLUDE_DIRS += ../../src/compiler
INCLUDE_DIRS += ../../vendor/1test

CC  = arm-none-eabi-gcc
CXX = arm-none-eabi-g++
AS  = arm-none-eabi-g++
LD  = arm-none-eabi-g++
OBJCOPY = arm-none-eabi-objcopy
OBJSIZE = arm-none-eabi-size

ASMFLAGS += -mcpu=cortex-m0 -mthumb

CXXFLAGS += -mcpu=cortex-m0 -mthumb -Os -DNDEBUG
CXXFLAGS += -fno-tree-loop-distribute-patterns
CXXFLAGS += -std=gnu++17 -fno-exceptions -fno-rtti

CXXFLAGS += -fstack-usage

# The same frames -fstack-usage reports, but as GCC's own callgraph: one .ci
# per translation unit (VCG), nodes keyed by mangled name and carrying their
# frame size, edges carrying the call site. Crucially an indirect call is not
# silently absent — it lands as an edge to a "__indirect_call" placeholder,
# so a new one has to be resolved deliberately rather than being missed.
CXXFLAGS += -fcallgraph-info=su,da

COMPILE_PROC_CPP_FLAGS += -Werror=stack-usage=512
TRANSLATE_PROC_CPP_FLAGS += -Werror=stack-usage=512
PROC_SCAN_CPP_FLAGS += -Werror=stack-usage=512
ABI_STRATEGY_CPP_FLAGS += -Werror=stack-usage=512
ASSEMBLER_CPP_FLAGS += -Werror=stack-usage=512

CXXFLAGS += -ffixed-r8 -ffixed-r9 -ffixed-r10 -ffixed-r11

CXXFLAGS += -fno-use-cxa-atexit

LDFLAGS += -mcpu=cortex-m0 -mthumb
LDFLAGS += -static -nostartfiles
LDFLAGS += -specs=nosys.specs
LDFLAGS += -T linker.ld

include ../../vendor/ultimate-makefile/Makefile.ultimate
$(OBJDIR)/$(abspath ../../src/runtime/runtime.S).o: $(abspath ../../src/runtime/runtime_host.h)

qemu-test: $(OUTPUT)
	qemu-system-arm -M lm3s811evb -m 8k -nographic -monitor none \
	    -semihosting-config enable=on,target=native -kernel $(OUTPUT)

qemu-debug: $(OUTPUT)
	qemu-system-arm -M lm3s811evb -m 8k -nographic -monitor none -s -S \
	    -semihosting-config enable=on,target=native -kernel $(OUTPUT)

TRANSLATE_MARGIN := $(shell sed -n 's/.*TRANSLATE_BODY_STACK_MARGIN = \([0-9]*\).*/\1/p' ../../src/compiler/translate_control_flow.cpp)

stack-usage-check: $(OUTPUT)
	../../tools/stack-margin.ts --guard 'jitc::Ctx::translate' --max $(TRANSLATE_MARGIN) \
	    'jitc::Ctx::translate' $(OBJDIR)

.PHONY: qemu-test qemu-debug stack-usage-check
