// BR_TABLE: the two-block fused dispatch, the shared jump-table helper past
// that, and the branch-range and accState edges of both.

#include <cstdint>

#include "instr.h"
#include "corpus_programs.h"
#include "run_program.h"
#include "Test.h"

using namespace jitc;

// ---- BR_TABLE if/else fusion, non-last case closed via a bare RETURN,
// last case closes normally.
static const Instr ifElseProc0[] = {
    LOAD(0), opImm(Op::GT_U, 10), brTable(1),
        CONST(111), bare(Op::RETURN),   // case 0 (n <= 10) — bare terminator
        CONST(222), bare(Op::BLOCK_END), // case 1 (n > 10) — normal close
    bare(Op::RETURN),
};

TEST(BrTableIfElseBelowTheBoundary)
{
    ProcSource procs[] = {PROC(1, ifElseProc0)};
    uint32_t args[] = {0};
    ProgramResult r = runProgram(procs, 1, args);

    CHECK(!r.trapped);
    CHECK(r.value == 111);
}

TEST(BrTableIfElseAtTheBoundary)
{
    ProcSource procs[] = {PROC(1, ifElseProc0)};
    uint32_t args[] = {10};
    ProgramResult r = runProgram(procs, 1, args);

    CHECK(!r.trapped);
    CHECK(r.value == 111);
}

TEST(BrTableIfElseAboveTheBoundary)
{
    ProcSource procs[] = {PROC(1, ifElseProc0)};
    uint32_t args[] = {11};
    ProgramResult r = runProgram(procs, 1, args);

    CHECK(!r.trapped);
    CHECK(r.value == 222);
}

// ---- BR_TABLE N>=2, the shared jump-table helper. Selector 3 is the
// default case, so it also catches everything above.
static const Instr jumpTableProc0[] = {
    LOAD(0), brTable(3),
        CONST(100), bare(Op::BLOCK_END),
        CONST(200), bare(Op::BLOCK_END),
        CONST(300), bare(Op::BLOCK_END),
        CONST(400), bare(Op::BLOCK_END),
    bare(Op::RETURN),
};

TEST(JumpTableSelectorZero)
{
    ProcSource procs[] = {PROC(1, jumpTableProc0)};
    uint32_t args[] = {0};
    ProgramResult r = runProgram(procs, 1, args);

    CHECK(!r.trapped);
    CHECK(r.value == 100);
}

TEST(JumpTableSelectorOne)
{
    ProcSource procs[] = {PROC(1, jumpTableProc0)};
    uint32_t args[] = {1};
    ProgramResult r = runProgram(procs, 1, args);

    CHECK(!r.trapped);
    CHECK(r.value == 200);
}

TEST(JumpTableSelectorTwo)
{
    ProcSource procs[] = {PROC(1, jumpTableProc0)};
    uint32_t args[] = {2};
    ProgramResult r = runProgram(procs, 1, args);

    CHECK(!r.trapped);
    CHECK(r.value == 300);
}

TEST(JumpTableSelectorThree)
{
    ProcSource procs[] = {PROC(1, jumpTableProc0)};
    uint32_t args[] = {3};
    ProgramResult r = runProgram(procs, 1, args);

    CHECK(!r.trapped);
    CHECK(r.value == 400);
}

TEST(JumpTableSelectorFarOutOfRangeTakesTheDefaultCase)
{
    ProcSource procs[] = {PROC(1, jumpTableProc0)};
    uint32_t args[] = {0xffffffffu};
    ProgramResult r = runProgram(procs, 1, args);

    CHECK(!r.trapped);
    CHECK(r.value == 400);
}

// ---- Branch-range guard forced into the long (invert-and-branch) form —
// case 0's own body is padded past the 240-byte safe span, so the dispatch
// guard itself can't be a bare short-form conditional branch.
static const Instr longGuardProc0[] = {
    LOAD(0), opImm(Op::GT_U, 100), brTable(1),
        bare(Op::NOT), bare(Op::NOT), bare(Op::NOT), bare(Op::NOT), bare(Op::NOT),
        bare(Op::NOT), bare(Op::NOT), bare(Op::NOT), bare(Op::NOT), bare(Op::NOT),
        bare(Op::NOT), bare(Op::NOT), bare(Op::NOT), bare(Op::NOT), bare(Op::NOT),
        bare(Op::NOT), bare(Op::NOT), bare(Op::NOT), bare(Op::NOT), bare(Op::NOT),
        CONST(1), bare(Op::RETURN),      // case 0 (n <= 100) — 20 NOTs + terminator, 21*16 = 336 > 240
        CONST(2), bare(Op::BLOCK_END),   // case 1 (n > 100) — normal
    bare(Op::RETURN),
};

TEST(LongFormBranchGuardTrueCase)
{
    ProcSource procs[] = {PROC(1, longGuardProc0)};
    uint32_t args[] = {0};
    ProgramResult r = runProgram(procs, 1, args);

    CHECK(!r.trapped);
    CHECK(r.value == 1);
}

TEST(LongFormBranchGuardFalseCase)
{
    ProcSource procs[] = {PROC(1, longGuardProc0)};
    uint32_t args[] = {200};
    ProgramResult r = runProgram(procs, 1, args);

    CHECK(!r.trapped);
    CHECK(r.value == 2);
}

// ---- Regression for armv6.h's isCondBranch, which excluded
// Condition::LE (0b1101) — this codebase's own largest valid condition — by
// checking `cond < 0b1101`. patchBranch treats any halfword isCondBranch
// rejects as unconditional and mis-patches it, crashing translateProc()
// outright — reachable via something as ordinary as `if (x <= 5)` (LE_S used
// directly as a BR_TABLE guard) or a `while (x > 0)` loop (GT_S's own inverse
// is LE). Two sub-cases in one procedure: an if/else guarded by LE_S,
// followed by a countdown loop whose exit condition is GT_S's inverse.
// BR_TABLE's own guard skips case 0 when the fused condition is true, so
// case 1 (200) fires for arg<=5 and case 0 (100) for arg>5; the loop always
// counts down to 0 and contributes nothing.
static const Instr condBranchLeProc0[] = {
    LOAD(0), opImm(Op::LE_S, 5), brTable(1),      // if/else guarded directly by LE_S
        CONST(100), bare(Op::BLOCK_END),
        CONST(200), bare(Op::BLOCK_END),
    PUSH(),                                        // slot1 = branch result
    LOAD(0), PUSH(),                                // slot2 = counter := arg
    bare(Op::LOOP),
        LOAD(2), opImm(Op::GT_S, 0),                // while(counter > 0) -- exit condition is GT_S's own inverse
    bare(Op::BLOCK_END),
        LOAD(2), opImm(Op::SUB, 1), STORE(2),
    bare(Op::BLOCK_END),
    LOAD(1), opReg(Op::ADD, 2),                     // branch result + counter(now 0)
    bare(Op::RETURN),
};

TEST(CondBranchLeBelowTheBoundary)
{
    ProcSource procs[] = {PROC(1, condBranchLeProc0)};
    uint32_t args[] = {3};
    ProgramResult r = runProgram(procs, 1, args);

    CHECK(!r.trapped);
    CHECK(r.value == 200);
}

TEST(CondBranchLeAtTheBoundary)
{
    ProcSource procs[] = {PROC(1, condBranchLeProc0)};
    uint32_t args[] = {5};
    ProgramResult r = runProgram(procs, 1, args);

    CHECK(!r.trapped);
    CHECK(r.value == 200);
}

TEST(CondBranchLeAboveTheBoundary)
{
    ProcSource procs[] = {PROC(1, condBranchLeProc0)};
    uint32_t args[] = {9};
    ProgramResult r = runProgram(procs, 1, args);

    CHECK(!r.trapped);
    CHECK(r.value == 100);
}

// ---- Regression for the accState-merge-boundary bug — emitComparison never
// materializes its 0/1 result into any register (only CPU flags carry it into
// the fused branch), so a case body's own accState was silently left
// describing whatever it held before the comparison ran, instead of the
// correct, statically-known "comparison was false"/"comparison was true"
// constant. A bare STORE right at the top of each case exposes it: slot 1
// starts at a stale sentinel (77, never 0/1), and each case's own probe
// should overwrite it with the comparison's real result instead.
static const Instr accStateMergeProc0[] = {
    CONST(77), PUSH(),                      // slot1 = 77 (stale sentinel)
    LOAD(0), opImm(Op::GE_U, 0x80), brTable(1),
        STORE(1), bare(Op::BLOCK_END),      // case 0 (false): probe
        STORE(1), bare(Op::BLOCK_END),      // case 1 (true): probe
    LOAD(1),
    bare(Op::RETURN),
};

TEST(AccStateMergeFalseCase)
{
    ProcSource procs[] = {PROC(1, accStateMergeProc0)};
    uint32_t args[] = {5};
    ProgramResult r = runProgram(procs, 1, args);

    CHECK(!r.trapped);
    CHECK(r.value == 0);
}

TEST(AccStateMergeTrueCase)
{
    ProcSource procs[] = {PROC(1, accStateMergeProc0)};
    uint32_t args[] = {200};
    ProgramResult r = runProgram(procs, 1, args);

    CHECK(!r.trapped);
    CHECK(r.value == 1);
}

// ---- Large BR_TABLE (N=20) with a CALL inside one case. A jump table with N
// well past 4 (stressing brTableJumpHelper's relocation math and the jump
// table's own literal-pool sizing at real scale) combined with a real CALL
// inside a case — re-exercises proc_scan.cpp's triggersLRSave/
// `(uint32_t)instr.imm > 2` fix end-to-end, since a large N combined with a
// real CALL is exactly the combination that bug could disagree on between the
// scan pass and the real translation pass. Each case STOREs to a result slot
// for the same reason LoopInBrTableCase* does. Body lives in
// corpus_programs.h.
TEST(LargeJumpTableCaseWithACall)
{
    ProcSource procs[] = {PROC(1, corpusLargeBrTableProc0), PROC(1, corpusLargeBrTableProc1)};
    uint32_t args[] = {7};
    ProgramResult r = runProgram(procs, 2, args);

    CHECK(!r.trapped);
    CHECK(r.value == 1005);
}

TEST(LargeJumpTableLowCase)
{
    ProcSource procs[] = {PROC(1, corpusLargeBrTableProc0), PROC(1, corpusLargeBrTableProc1)};
    uint32_t args[] = {3};
    ProgramResult r = runProgram(procs, 2, args);

    CHECK(!r.trapped);
    CHECK(r.value == 30);
}

TEST(LargeJumpTableLastCase)
{
    ProcSource procs[] = {PROC(1, corpusLargeBrTableProc0), PROC(1, corpusLargeBrTableProc1)};
    uint32_t args[] = {19};
    ProgramResult r = runProgram(procs, 2, args);

    CHECK(!r.trapped);
    CHECK(r.value == 190);
}

// ---- isa-core.md §8.7's merge: the dispatch is total, so every edge into
// the merge is a case body and a value crosses it — the `+ 1` below reads
// what the taken arm left in acc, which neither arm stored anywhere.
static const Instr accMergeProc0[] = {
    LOAD(0), opImm(Op::GT_U, 3), brTable(1),
        CONST(11), bare(Op::BLOCK_END),
        CONST(22), bare(Op::BLOCK_END),
    opImm(Op::ADD, 1), bare(Op::RETURN),
};

TEST(DispatchMergeCarriesAccOutOfTheTrueArm)
{
    ProcSource procs[] = {PROC(1, accMergeProc0)};
    uint32_t args[] = {9}; // 9 > 3 -> acc 1 -> case[1]
    ProgramResult r = runProgram(procs, 1, args);

    CHECK(!r.trapped);
    CHECK(r.value == 23);
}

TEST(DispatchMergeCarriesAccOutOfTheFalseArm)
{
    ProcSource procs[] = {PROC(1, accMergeProc0)};
    uint32_t args[] = {1}; // 1 > 3 is false -> acc 0 -> case[0]
    ProgramResult r = runProgram(procs, 1, args);

    CHECK(!r.trapped);
    CHECK(r.value == 12);
}

// Unfused: the dispatch value is a plain load, not a comparison, so it is
// any word at all — which the truthy test still splits without a range
// check, because at N=1 everything above 0 is one outcome.
static const Instr unfusedProc0[] = {
    LOAD(0), brTable(1),
        CONST(5), bare(Op::BLOCK_END),
        CONST(6), bare(Op::BLOCK_END),
    bare(Op::RETURN),
};

TEST(TwoBlockDispatchUnfusedZero)
{
    ProcSource procs[] = {PROC(1, unfusedProc0)};
    uint32_t args[] = {0};
    ProgramResult r = runProgram(procs, 1, args);

    CHECK(!r.trapped);
    CHECK(r.value == 5);
}

TEST(TwoBlockDispatchUnfusedFarAboveOne)
{
    ProcSource procs[] = {PROC(1, unfusedProc0)};
    uint32_t args[] = {0x9000u}; // everything at or above N is case[N]
    ProgramResult r = runProgram(procs, 1, args);

    CHECK(!r.trapped);
    CHECK(r.value == 6);
}

// ---- FALLTHROUGH in the two-block form: case[0] runs on into case[1], so
// the merge has one incoming edge instead of two.
static const Instr fallProc0[] = {
    LOAD(0), opImm(Op::EQ, 0), brTable(1),
        bare(Op::FALLTHROUGH),
        CONST(7), bare(Op::BLOCK_END),
    bare(Op::RETURN),
};

TEST(TwoBlockDispatchFallthroughFromEitherArm)
{
    ProcSource procs[] = {PROC(1, fallProc0)};
    for(uint32_t arg = 0; arg < 2; arg++)
    {
        uint32_t args[] = {arg};
        ProgramResult r = runProgram(procs, 1, args);

        CHECK(!r.trapped);
        CHECK(r.value == 7);
    }
}

// ---- C's `case 0: case 1: X` — a lone FALLTHROUGH sharing the next
// case's body, in a jump-table dispatch with an empty default case.
static const Instr sharedBodyProc0[] = {
    CONST(0), PUSH(),
    LOAD(0), brTable(3),
        bare(Op::FALLTHROUGH),
        CONST(10), STORE(1), bare(Op::BLOCK_END),
        CONST(20), STORE(1), bare(Op::BLOCK_END),
        bare(Op::BLOCK_END),
    LOAD(1), bare(Op::RETURN),
};

TEST(SwitchSharedCaseBody)
{
    const uint32_t expected[] = {10, 10, 20, 0};
    for(uint32_t arg = 0; arg < 4; arg++)
    {
        ProcSource procs[] = {PROC(1, sharedBodyProc0)};
        uint32_t args[] = {arg};
        ProgramResult r = runProgram(procs, 1, args);

        CHECK(!r.trapped);
        CHECK(r.value == expected[arg]);
    }
}
