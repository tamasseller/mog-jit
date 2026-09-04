OUTPUT = run.elf

SOURCES += vectors.S

# The translator and the real runtime, entry layer and all.
include ../../src/compiler/compiler-all.mk
include ../../src/compiler/emit/ext-default.mk
include ../../src/runtime/runtime-all.mk
include ../../src/runtime/executor.mk
include ../../src/runtime/dispatch.mk
include ../../src/runtime/bytecode-default.mk

SOURCES += ../encode_instr.cpp
SOURCES += ../ext_rawmem.cpp
SOURCES += ../ext_rawmem_helper.S
SOURCES += test_calls.cpp
SOURCES += test_data_flow.cpp
SOURCES += test_unary_ops.cpp
SOURCES += test_loops.cpp
SOURCES += test_br_table.cpp
SOURCES += test_nested_blocks.cpp
SOURCES += test_literal_pool.cpp
SOURCES += test_trap_unwind.cpp
SOURCES += test_entry_args.cpp
SOURCES += test_program_rejection.cpp
SOURCES += test_eviction.cpp
SOURCES += test_stack_budget.cpp
SOURCES += test_ext_rawmem.cpp
SOURCES += test_cancel.cpp
SOURCES += test_extend_ops.cpp
SOURCES += stack_paint.cpp
SOURCES += semihosting_output.cpp
SOURCES += cxx_stubs.cpp
SOURCES += main.cpp

SOURCES += ../../vendor/1test/TestRunner.cpp

INCLUDE_DIRS += .
INCLUDE_DIRS += ..
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

# 1test embeds __FILE__ ":" __LINE__ once per TEST and once per CHECK; at
# absolute paths those strings alone cost several KB of the 64KB image.
CXXFLAGS += -ffile-prefix-map=$(abspath ../../..)/=

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
$(OBJDIR)/$(abspath ../../src/runtime/runtime.S).o: $(abspath ../../src/runtime/dispatch_abi.h)

qemu-test: $(OUTPUT)
	qemu-system-arm -M microbit -nographic -monitor none \
	    -semihosting-config enable=on,target=native -kernel $(OUTPUT)

qemu-debug: $(OUTPUT)
	qemu-system-arm -M microbit -nographic -monitor none -s -S \
	    -semihosting-config enable=on,target=native -kernel $(OUTPUT)

BUDGET_H := ../../src/runtime/stack_budget.h
SCAN_MARGIN := $(shell sed -n 's/.*SCAN_STACK_MARGIN \([0-9]*\).*/\1/p' $(BUDGET_H))
ENTRY_MARGIN := $(shell sed -n 's/.*TRANSLATOR_ENTRY_CPP_BYTES \([0-9]*\).*/\1/p' $(BUDGET_H))
TRANSLATE_MARGIN := $(shell sed -n 's/.*TRANSLATE_LEVEL_STACK_MARGIN \([0-9]*\).*/\1/p' $(BUDGET_H))

stack-usage-check: $(OUTPUT)
	../../tools/stack-margin.ts --guard 'GUARDED_' --max $(TRANSLATE_MARGIN) 'GUARDED_processUntilTerminator' $(OBJDIR)
	../../tools/stack-margin.ts --guard 'GUARDED_' --max $(SCAN_MARGIN) 'GUARDED_scanBody' $(OBJDIR)
	../../tools/stack-margin.ts --guard 'GUARDED_' --max $(ENTRY_MARGIN) 'translateProc|scanProcBody' $(OBJDIR)

.PHONY: qemu-test qemu-debug stack-usage-check
