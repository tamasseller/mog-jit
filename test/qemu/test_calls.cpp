// Calls: argument marshalling into a callee, call chains, and the
// savesLR return paths a non-leaf procedure with out-of-window arguments
// takes.

#include <cstdint>

#include "instr.h"
#include "run_program.h"
#include "Test.h"

using namespace jitc;

// ---- Single-argument call, entirely acc-passed.
static const Instr singleArgCallProc0[] = {CONST(37), call(1), bare(Op::RETURN)};
static const Instr singleArgCallProc1[] = {LOAD(0), opImm(Op::ADD, 5), bare(Op::RETURN)};

TEST(SingleArgumentCall)
{
    ProcSource procs[] = {PROC(0, singleArgCallProc0), PROC(1, singleArgCallProc1)};
    ProgramResult r = runProgram(procs, 2, nullptr);

    CHECK(!r.trapped);
    CHECK(r.value == 42);
}

// ---- 3-argument call with a phase-misaligned shuffle and surviving
// leftover locals.
static const Instr shuffleProc0[] = {
    CONST(100), PUSH(), // leftover local 0 -- k=0
    CONST(200), PUSH(), // leftover local 1 -- k=1
    CONST(300), PUSH(), // leftover local 2 -- k=2
    CONST(10), PUSH(),  // stack arg 0 for the callee -- k=3
    CONST(20), PUSH(),  // stack arg 1 for the callee -- k=4
    CONST(999),         // last (acc) arg -- never pushed
    call(1),
    opReg(Op::ADD, 0),
    opReg(Op::ADD, 1),
    opReg(Op::ADD, 2),
    bare(Op::RETURN),
};
static const Instr shuffleProc1[] = {LOAD(0), opReg(Op::ADD, 1), opReg(Op::ADD, 2), bare(Op::RETURN)};

TEST(PhaseMisalignedArgumentShuffle)
{
    ProcSource procs[] = {PROC(0, shuffleProc0), PROC(3, shuffleProc1)};
    ProgramResult r = runProgram(procs, 2, nullptr);

    CHECK(!r.trapped);
    CHECK(r.value == 1629);
}

// ---- CALL with stackArgs(6) > WINDOW_SIZE(4).
static const Instr wideCallProc0[] = {
    CONST(10), PUSH(), // arg0
    CONST(20), PUSH(), // arg1
    CONST(30), PUSH(), // arg2
    CONST(40), PUSH(), // arg3
    CONST(50), PUSH(), // arg4
    CONST(60), PUSH(), // arg5
    CONST(70),          // arg6 -- last arg, via acc, never pushed
    call(1),
    bare(Op::RETURN),
};
static const Instr wideCallProc1[] = {
    LOAD(0),
    opReg(Op::ADD, 1), opReg(Op::ADD, 2), opReg(Op::ADD, 3),
    opReg(Op::ADD, 4), opReg(Op::ADD, 5), opReg(Op::ADD, 6),
    bare(Op::RETURN),
};

TEST(MoreStackArgumentsThanTheWindowHolds)
{
    ProcSource procs[] = {PROC(0, wideCallProc0), PROC(7, wideCallProc1)};
    ProgramResult r = runProgram(procs, 2, nullptr);

    CHECK(!r.trapped);
    CHECK(r.value == 280);
}

// ---- A 3-deep call chain.
static const Instr chainProc0[] = {CONST(5), call(1), bare(Op::RETURN)};
static const Instr chainProc1[] = {LOAD(0), call(2), opImm(Op::ADD, 1), bare(Op::RETURN)};
static const Instr chainProc2[] = {LOAD(0), opImm(Op::ADD, 100), bare(Op::RETURN)};

TEST(ThreeDeepCallChain)
{
    ProcSource procs[] = {PROC(0, chainProc0), PROC(1, chainProc1), PROC(1, chainProc2)};
    ProgramResult r = runProgram(procs, 3, nullptr);

    CHECK(!r.trapped);
    CHECK(r.value == 106);
}

// ---- A non-leaf procedure (proc1, argCount=5) with an out-of-window
// argument (k=0, argCount > WINDOW_SIZE(4)) sitting below its own pushed
// call/return record — the abiEmitReturn/returnHelperFromStackReclaim path
// (savesLR && initialSpilledCount > 0). k=0 is read after proc1's own nested
// call returns, exercising spillOffset's savesLR shift for a live read, not
// just the reclaim at RETURN.
static const Instr savesLrReturnProc0[] = {
    CONST(1), PUSH(),  // arg0 for proc1 -- k=0, proc1's out-of-window arg
    CONST(2), PUSH(),  // arg1 -- k=1
    CONST(3), PUSH(),  // arg2 -- k=2
    CONST(4), PUSH(),  // arg3 -- k=3
    CONST(500),         // arg4 -- last, via acc
    call(1),
    bare(Op::RETURN),
};
static const Instr savesLrReturnProc1[] = {
    LOAD(0),              // acc = arg0 (k=0, out-of-window)
    call(2),               // proc2(arg0) -- makes proc1 non-leaf (savesLR)
    opReg(Op::ADD, 4),       // acc += arg4 (k=4, still in-window)
    bare(Op::RETURN),
};
static const Instr savesLrReturnProc2[] = {LOAD(0), opImm(Op::ADD, 1000), bare(Op::RETURN)};

TEST(SavesLrReturnWithOutOfWindowArgs)
{
    ProcSource procs[] = {PROC(0, savesLrReturnProc0), PROC(5, savesLrReturnProc1), PROC(1, savesLrReturnProc2)};
    ProgramResult r = runProgram(procs, 3, nullptr);

    CHECK(!r.trapped);
    CHECK(r.value == 1501); // proc2(1) = 1001, + arg4(500)
}

// ---- Two-level savesLR/returnHelperFromStackReclaim chain, extending
// SavesLrReturnWithOutOfWindowArgs' shape (one non-leaf procedure with an
// out-of-window arg below its own pushed call record) to two nested levels:
// proc0 -> proc1 (argCount=5, out-of-window arg, calls proc2) -> proc2
// (argCount=5, out-of-window arg, calls proc3) -> proc3 (leaf).
static const Instr twoLevelProc0[] = {
    CONST(1), PUSH(),  // arg0 for proc1 -- k=0, proc1's out-of-window arg
    CONST(2), PUSH(),  // arg1 -- k=1
    CONST(3), PUSH(),  // arg2 -- k=2
    CONST(4), PUSH(),  // arg3 -- k=3
    CONST(50),          // arg4 -- last, via acc
    call(1),
    bare(Op::RETURN),
};
static const Instr twoLevelProc1[] = {
    LOAD(0),               // acc = arg0 (k=0, out-of-window)
    PUSH(),                 // k=5 = saved copy of proc1's own arg0, survives across the nested call
    CONST(10), PUSH(),      // arg0 for proc2 -- k=6, proc2's own out-of-window arg
    CONST(11), PUSH(),      // arg1 -- k=7
    CONST(12), PUSH(),      // arg2 -- k=8
    CONST(13), PUSH(),      // arg3 -- k=9
    CONST(500),              // arg4 -- last, via acc
    call(2),                  // proc2(10,11,12,13,500) -- makes proc1 non-leaf (savesLR)
    opReg(Op::ADD, 5),         // acc += proc1's own saved arg0 (k=5)
    opReg(Op::ADD, 4),          // acc += arg4 (k=4, still in-window)
    bare(Op::RETURN),
};
static const Instr twoLevelProc2[] = {
    LOAD(0),           // acc = arg0 (k=0, out-of-window)
    call(3),            // proc3(arg0) -- makes proc2 non-leaf (savesLR)
    opReg(Op::ADD, 4),   // acc += arg4 (k=4, still in-window)
    bare(Op::RETURN),
};
static const Instr twoLevelProc3[] = {LOAD(0), opImm(Op::ADD, 1000), bare(Op::RETURN)};

TEST(TwoLevelSavesLrChain)
{
    ProcSource procs[] = {
        PROC(0, twoLevelProc0), PROC(5, twoLevelProc1), PROC(5, twoLevelProc2), PROC(1, twoLevelProc3),
    };
    ProgramResult r = runProgram(procs, 4, nullptr);

    CHECK(!r.trapped);
    // proc3(10)=1010; proc2 = 1010+arg4(500) = 1510; proc1 = 1510+ownArg0(1)+ownArg4(50).
    CHECK(r.value == 1561);
}
