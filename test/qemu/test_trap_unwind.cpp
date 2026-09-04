// A bytecode TRAP unwinds to the landing from any call depth (isa-core.md §4.5/§9).

#include <cstdint>

#include "instr.h"
#include "run_program.h"
#include "dispatch_abi.h"
#include "Test.h"

using namespace jitc;

// ---- A bytecode TRAP really unwinds (isa-core.md §4.5/§9). This is the
// four-instruction program fuzz/src/qemu-exec minimized the original
// 195-instruction finding down to: the trap sits in a *nested* procedure, so
// a TRAP compiled as an ordinary return handed 0x800002f2 to proc0 as a
// return value and proc0 went on to return 92. Now it reaches enterDispatch's
// landing directly, tagged LANDING_TRAP, with the code untouched in value.
static const Instr nestedTrapProc0[] = {call(1), CONST(92), bare(Op::RETURN)};
static const Instr nestedTrapProc1[] = {trapInstr(754)};

TEST(TrapInANestedProcedureUnwinds)
{
    ProcSource procs[] = {PROC(0, nestedTrapProc0), PROC(0, nestedTrapProc1)};
    ProgramResult r = runProgram(procs, 2, nullptr);

    CHECK(r.trapped == LANDING_TRAP);
    CHECK(r.value == 754);
}

// ---- TRAP in the *entry* procedure, the one case the old sentinel encoding
// got right — kept because it is now right for a different reason (the tag,
// not bit 31 of the value), and because five live pushed locals mean sp is
// nowhere near its entry value when the trap fires. trapHelper's own
// `mov sp, savedSp` is what makes that irrelevant; the old code needed
// discardWindow first.
static const Instr entryTrapProc0[] = {
    CONST(1), PUSH(), CONST(2), PUSH(), CONST(3), PUSH(), CONST(4), PUSH(), CONST(5), PUSH(),
    trapInstr(41),
};

TEST(TrapInTheEntryProcedureUnwinds)
{
    ProcSource procs[] = {PROC(0, entryTrapProc0)};
    ProgramResult r = runProgram(procs, 1, nullptr);

    CHECK(r.trapped == LANDING_TRAP);
    CHECK(r.value == 41);
}

// ---- The worst frame shape to unwind out of — proc1 has argCount 5, so its
// own arg0 sits out of window *below* the call record its prologue pushed
// (abi_strategy.cpp's returnHelperFromStackReclaim case), and both frames
// have live pushed locals at the moment proc2 traps two levels down. Nothing
// on any of the three frames is unwound a step at a time; the single sp
// restore subsumes all of it.
static const Instr deepTrapProc0[] = {
    CONST(7), PUSH(),   // k0 -- live across the call, never read again
    CONST(10), PUSH(),  // arg0 for proc1 -- k=1, proc1's own out-of-window arg
    CONST(11), PUSH(),  // arg1 -- k=2
    CONST(12), PUSH(),  // arg2 -- k=3
    CONST(13), PUSH(),  // arg3 -- k=4
    CONST(14),          // arg4 -- last, via acc
    call(1),
    opReg(Op::ADD, 0), bare(Op::RETURN), // unreachable once proc2 traps
};
static const Instr deepTrapProc1[] = {
    LOAD(0), PUSH(),    // k5 -- proc1's own saved copy of its out-of-window arg0
    CONST(21),          // proc2's only argument, via acc
    call(2),            // makes proc1 non-leaf: its prologue pushes the record
    bare(Op::RETURN),   // unreachable
};
static const Instr deepTrapProc2[] = {trapInstr(1000)};

TEST(TrapTwoLevelsDownUnwinds)
{
    ProcSource procs[] = {PROC(0, deepTrapProc0), PROC(5, deepTrapProc1), PROC(1, deepTrapProc2)};
    ProgramResult r = runProgram(procs, 3, nullptr);

    CHECK(r.trapped == LANDING_TRAP);
    CHECK(r.value == 1000);
}
