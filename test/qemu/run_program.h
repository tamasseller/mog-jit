// Runs one whole encoded program (the packages/machine/src/bytecode.ts
// envelope plus an ordinary isa-core.md §5.5 body) on the real target — what
// every test_*.cpp file covering translated-program behaviour is written in
// terms of.
#ifndef JIT_ARMV6M_TEST_QEMU_RUN_PROGRAM_H_
#define JIT_ARMV6M_TEST_QEMU_RUN_PROGRAM_H_

#include <cstdint>

#include "instr.h"
#include "encode_instr.h"
#include "executor.h"

extern "C" uint8_t __bss_end;

// Instr[]'s own element count paired with its own argCount — one ProcSource
// per procedure.
#define PROC(argCount, body) ProcSource{argCount, body, sizeof(body) / sizeof(body[0])}

namespace jitc
{

inline constexpr uint32_t ARENA_BYTES = 400;
inline constexpr uint32_t STACK_SLACK_ABOVE_BSS = 128;

// The longest encoded program in these tests measures 126 bytes; -DNDEBUG
// strips encodeInstr's own overrun assert, so keep the margin real.
inline constexpr uint32_t PROGRAM_CAPACITY = 256;

// max_call_depth 0 and total_depth = the entry procedure's own arg_count: so
// slack an envelope that Executor::run's up-front budget check sees almost no
// operand-stack or call-record cost and can never reject. test_stack_budget.cpp
// is what exercises that check against real, hand-derived figures. Not zero,
// though: enterProgramCore refuses to push a multi-argument entry procedure's
// out-of-window arguments past whatever total_depth claims, and arg_count is
// the lower bound validateProgram itself guarantees.
inline ProgramResult runProgram(const ProcSource *procs, uint32_t procCount, uint32_t *args)
{
    static uint8_t arena[ARENA_BYTES];

    uint8_t bytes[PROGRAM_CAPACITY];
    const uint32_t len = encodeJitProgram(0, procs[0].argCount, procs, procCount, bytes, sizeof(bytes));

    return Executor::split((uint32_t)(uintptr_t)arena, ARENA_BYTES,
            (uint32_t)(uintptr_t)&__bss_end + STACK_SLACK_ABOVE_BSS, /*interruptReserve=*/0)
        .run(bytes, len, args, procs[0].argCount);
}

} // namespace jitc

#endif // JIT_ARMV6M_TEST_QEMU_RUN_PROGRAM_H_
