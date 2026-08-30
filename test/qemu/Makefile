OUTPUT = run.elf

SOURCES += vectors.S

SOURCES += ../../runtime/runtime.S
SOURCES += ../../runtime/runtime.cpp
SOURCES += ../../runtime/enter_program.cpp
SOURCES += ../../runtime/dispatch_abi.cpp

# The compiler itself
SOURCES += ../../compiler/src/window.cpp
SOURCES += ../../compiler/src/ext.cpp
SOURCES += ../../compiler/src/accstate.cpp
SOURCES += ../../compiler/src/arithmetic.cpp
SOURCES += ../../compiler/src/assembler.cpp
SOURCES += ../../compiler/src/shape.cpp
SOURCES += ../../compiler/src/abi_strategy.cpp
SOURCES += ../../compiler/src/decode_instr.cpp
SOURCES += ../../compiler/src/proc_scan.cpp
SOURCES += ../../compiler/src/translate_proc.cpp
SOURCES += ../../compiler/src/translate_data_flow.cpp
SOURCES += ../../compiler/src/translate_control_flow.cpp

SOURCES += ../encode_instr.cpp
SOURCES += fixtures.cpp
SOURCES += smoke_test.cpp
SOURCES += stack_paint.cpp
SOURCES += semihosting_output.cpp
SOURCES += cxx_stubs.cpp
SOURCES += main.cpp

SOURCES += ../../vendor/1test/TestRunner.cpp

INCLUDE_DIRS += ../../compiler/src   # jitc:: headers, armv6.h
INCLUDE_DIRS += ../../runtime         # runtime_host.h / runtime.h
INCLUDE_DIRS += .                      # fixtures.h / semihosting_output.h
INCLUDE_DIRS += ..                      # corpus_programs.h, encode_instr.h
INCLUDE_DIRS += ../../vendor/1test      # Test.h

CC  = arm-none-eabi-gcc
CXX = arm-none-eabi-g++
AS  = arm-none-eabi-g++
LD  = arm-none-eabi-g++
OBJCOPY = arm-none-eabi-objcopy
OBJSIZE = arm-none-eabi-size

ASMFLAGS += -mcpu=cortex-m0 -mthumb

CXXFLAGS += -mcpu=cortex-m0 -mthumb -Os -DNDEBUG
CXXFLAGS += -std=gnu++17 -fno-exceptions -fno-rtti

CXXFLAGS += -fstack-usage

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
$(OBJDIR)/$(abspath ../../runtime/runtime.S).o: $(abspath ../../runtime/runtime_host.h)

qemu-test: $(OUTPUT)
	qemu-system-arm -M lm3s811evb -m 8k -nographic -monitor none \
	    -semihosting-config enable=on,target=native -kernel $(OUTPUT)

qemu-debug: $(OUTPUT)
	qemu-system-arm -M lm3s811evb -m 8k -nographic -monitor none -s -S \
	    -semihosting-config enable=on,target=native -kernel $(OUTPUT)

stack-usage-check: $(OUTPUT)
	python3 check_stack_usage.py $(OBJDIR)

.PHONY: qemu-test qemu-debug stack-usage-check
