// DROP and DEFAULT on real hardware: the two instructions isa-core.md §4.4
// and §4.5 added for a scope that no block boundary closes, and for a
// dispatch case that continues into `case[N]` rather than the next one.

#include <cstdint>

#include "instr.h"
#include "registers.h"
#include "run_program.h"
#include "Test.h"

using namespace jitc;

// ---- DROP reclaims slots, and the next PUSH genuinely reuses them.
// k1..k3 are pushed and dropped, then k1 is pushed again and read back —
// so the window has to have unwound, not merely renumbered.
static const Instr dropReusesSlotsProc0[] = {
    LOAD(0), PUSH(),                    // k1 = arg
    CONST(11), PUSH(),                  // k2
    CONST(22), PUSH(),                  // k3
    dropInstr(2),                       // k2, k3 gone
    CONST(33), PUSH(),                  // lands back on k2's own slot
    LOAD(2), opReg(Op::ADD, 1),         // 33 + arg
    bare(Op::RETURN),
};

TEST(DropReclaimsSlotsAndThePushAfterItReusesThem)
{
    ProcSource procs[] = {PROC(1, dropReusesSlotsProc0)};
    uint32_t args[] = {9};
    ProgramResult r = runProgram(procs, 1, args);

    CHECK(!r.trapped);
    CHECK(r.value == 42);
}

// ---- DROP across the physical-register window's own edge. WINDOW_SIZE is
// 4 (registers.h), so pushing past it spills and dropping back under it has
// to reload — the same Window::restore path a BLOCK_END takes, reached
// through an instruction instead.
static const Instr dropAcrossWindowEdgeProc0[] = {
    CONST(1), PUSH(), CONST(2), PUSH(), CONST(3), PUSH(),
    CONST(4), PUSH(), CONST(5), PUSH(), CONST(6), PUSH(),
    dropInstr(4),                       // back to two live slots, from six
    LOAD(0), opReg(Op::ADD, 1),         // 1 + 2
    bare(Op::RETURN),
};

TEST(DropUnwindsPastTheRegisterWindowEdge)
{
    ProcSource procs[] = {PROC(0, dropAcrossWindowEdgeProc0)};
    ProgramResult r = runProgram(procs, 1, nullptr);

    CHECK(!r.trapped);
    CHECK(r.value == 3);
}

// ---- DROP's extended form (n >= 5, §5.4's own bias).
static const Instr dropExtendedFormProc0[] = {
    CONST(7), PUSH(),
    CONST(1), PUSH(), CONST(2), PUSH(), CONST(3), PUSH(),
    CONST(4), PUSH(), CONST(5), PUSH(), CONST(6), PUSH(),
    dropInstr(6),
    LOAD(0), bare(Op::RETURN),
};

TEST(DropsExtendedFormReclaimsEverythingItNames)
{
    ProcSource procs[] = {PROC(0, dropExtendedFormProc0)};
    ProgramResult r = runProgram(procs, 1, nullptr);

    CHECK(!r.trapped);
    CHECK(r.value == 7);
}

// ---- DEFAULT out of a two-block dispatch, where the case it names is the
// physically next one — the shape a one-label `switch` with a `default:`
// clause produces.
static const Instr defaultTwoBlockProc0[] = {
    CONST(0), PUSH(),                   // k1 = result
    LOAD(0), brTable(1),
        CONST(10), STORE(1), bare(Op::DEFAULT), // runs on into case[1]
        LOAD(1), opImm(Op::ADD, 1), STORE(1), bare(Op::BLOCK_END),
    LOAD(1), bare(Op::RETURN),
};

TEST(DefaultOutOfTheTwoBlockFormRunsTheCaseItNames)
{
    ProcSource procs[] = {PROC(1, defaultTwoBlockProc0)};
    uint32_t zero[] = {0};
    ProgramResult r = runProgram(procs, 1, zero);

    CHECK(!r.trapped);
    CHECK(r.value == 11); // case[0] stored 10, then ran on into case[1]
}

TEST(DefaultLeavesTheOtherCaseAlone)
{
    ProcSource procs[] = {PROC(1, defaultTwoBlockProc0)};
    uint32_t one[] = {1};
    ProgramResult r = runProgram(procs, 1, one);

    CHECK(!r.trapped);
    CHECK(r.value == 1); // straight to case[1], off a result still 0
}

// ---- DEFAULT out of a jump-table dispatch, where the case it names is
// several blocks further on: the forward branch translateSwitch chains and
// patches when case[N] finally starts. Cases 1 and 3 are gap fillers.
static const Instr defaultJumpTableProc0[] = {
    CONST(0), PUSH(),                   // k1 = result
    LOAD(0), brTable(4),
        CONST(100), STORE(1), bare(Op::BLOCK_END),
        bare(Op::DEFAULT),
        CONST(200), STORE(1), bare(Op::DEFAULT), // stores, then runs the default too
        bare(Op::DEFAULT),
        LOAD(1), opImm(Op::ADD, 7), STORE(1), bare(Op::BLOCK_END), // case[N]
    LOAD(1), bare(Op::RETURN),
};

static uint32_t dispatchWith(uint32_t k)
{
    ProcSource procs[] = {PROC(1, defaultJumpTableProc0)};
    uint32_t args[] = {k};
    ProgramResult r = runProgram(procs, 1, args);

    CHECK(!r.trapped);
    return r.value;
}

TEST(DefaultFromAGapFillerReachesTheDefaultCase)
{
    CHECK(dispatchWith(1) == 7);
    CHECK(dispatchWith(3) == 7);
}

TEST(DefaultFromANonEmptyCaseRunsItsOwnBodyFirst)
{
    CHECK(dispatchWith(2) == 207);
}

TEST(TheOtherCasesOfADefaultCarryingTableAreUnaffected)
{
    CHECK(dispatchWith(0) == 100);
    CHECK(dispatchWith(4) == 7);   // out of range, straight to case[N]
    CHECK(dispatchWith(99) == 7);
}
