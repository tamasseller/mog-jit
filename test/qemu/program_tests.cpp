// Whole encoded programs (the packages/machine/src/bytecode.ts envelope plus
// an ordinary isa-core.md §5.5 body) run end to end on the real target: one
// TEST per scenario, each encoding its own program and checking the landing
// Executor::split comes back with.

#include <cstdint>

#include "instr.h"
#include "encode_instr.h"
#include "corpus_programs.h"
#include "executor.h"
#include "dispatch_abi.h"
#include "Test.h"

using namespace jitc;

extern "C" uint8_t __bss_end;

static constexpr uint32_t ARENA_BYTES = 400;
static constexpr uint32_t STACK_SLACK_ABOVE_BSS = 128;
static uint8_t arena[ARENA_BYTES];

// The longest encoded program here measures 126 bytes; -DNDEBUG strips
// encodeInstr's own overrun assert, so keep the margin real.
static constexpr uint32_t PROGRAM_CAPACITY = 256;

#define PROC(argCount, body) ProcSource{argCount, body, sizeof(body) / sizeof(body[0])}

// max_call_depth 0 and total_depth = the entry procedure's own arg_count: so
// slack an envelope that Executor::run's up-front budget check sees almost no
// operand-stack or call-record cost and can never reject. main.cpp's own
// Executor scenarios are what exercise that check against real, hand-derived
// figures. Not zero, though: enterProgramCore refuses to push a multi-argument
// entry procedure's out-of-window arguments past whatever total_depth claims,
// and arg_count is the lower bound validateProgram itself guarantees.
static ProgramResult runProgram(const ProcSource *procs, uint32_t procCount, uint32_t *args)
{
    uint8_t bytes[PROGRAM_CAPACITY];
    const uint32_t len = encodeJitProgram(0, procs[0].argCount, procs, procCount, bytes, sizeof(bytes));

    return Executor::split((uint32_t)(uintptr_t)arena, ARENA_BYTES,
            (uint32_t)(uintptr_t)&__bss_end + STACK_SLACK_ABOVE_BSS, /*interruptReserve=*/0)
        .run(bytes, len, args, procs[0].argCount);
}

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

// ---- Out-of-window LOAD/STORE/REG_ACC/REG_REG, no CALL.
static const Instr outOfWindowProc0[] = {
    CONST(10), PUSH(), // k=0
    CONST(20), PUSH(), // k=1
    CONST(30), PUSH(), // k=2
    CONST(40), PUSH(), // k=3
    CONST(50), PUSH(), // k=4 -- evicts k=0's register
    CONST(60), PUSH(), // k=5 -- evicts k=1's register; k=0,1 now spilled
    LOAD(0),                    // acc = 10 (out-of-window LOAD)
    opReg(Op::ADD, 1),           // acc = 10+20=30 (out-of-window REG_ACC operand)
    STORE(0),                     // k0 := 30 (out-of-window STORE)
    CONST(5),
    opRegWriteback(Op::ADD, 1),    // k1 := 5+20=25 (out-of-window REG_REG)
    LOAD(0),                        // acc = k0 = 30
    opReg(Op::ADD, 1),                // acc = 30 + k1(25) = 55
    bare(Op::RETURN),
};

TEST(OutOfWindowLoadStoreAndRegisterOperands)
{
    ProcSource procs[] = {PROC(0, outOfWindowProc0)};
    ProgramResult r = runProgram(procs, 1, nullptr);

    CHECK(!r.trapped);
    CHECK(r.value == 55);
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

// ---- Operand-fold.
static const Instr operandFoldProc0[] = {
    CONST(10), PUSH(), // a -- k=0
    CONST(20), PUSH(), // b -- k=1
    CONST(30), PUSH(), // c -- k=2
    CONST(40), PUSH(), // d -- k=3
    LOAD(0),             // acc = a; accState depends on physReg(0)
    PUSH(),                // e = a -- k=4, evicts k=0's register
    bare(Op::RETURN),
};

TEST(RotationOperandFold)
{
    ProcSource procs[] = {PROC(0, operandFoldProc0)};
    ProgramResult r = runProgram(procs, 1, nullptr);

    CHECK(!r.trapped);
    CHECK(r.value == 10);
}

// ---- Destination-fold.
static const Instr destinationFoldProc0[] = {
    CONST(10), PUSH(),
    CONST(20), PUSH(),
    CONST(30), PUSH(),
    CONST(40), PUSH(),
    CONST(99), STORE(0), // a := 99; accState depends on physReg(0)
    PUSH(),                // e = a (now 99) -- k=4, evicts k=0's register
    bare(Op::RETURN),
};

TEST(RotationDestinationFold)
{
    ProcSource procs[] = {PROC(0, destinationFoldProc0)};
    ProgramResult r = runProgram(procs, 1, nullptr);

    CHECK(!r.trapped);
    CHECK(r.value == 99);
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

// ---- LOOP body closed by a bare terminator, not BLOCK_END.
static const Instr loopClosedByReturnProc0[] = {
    bare(Op::LOOP), bare(Op::BLOCK_END),  // condition sub-block is empty — testAccNonzero(arg)
    CONST(42), bare(Op::RETURN),           // body — bare terminator closes it
    CONST(999), bare(Op::RETURN),          // reached only via the cond-false exit
};

TEST(LoopClosedByReturnTakesTheCondFalseExit)
{
    ProcSource procs[] = {PROC(1, loopClosedByReturnProc0)};
    uint32_t args[] = {0};
    ProgramResult r = runProgram(procs, 1, args);

    CHECK(!r.trapped);
    CHECK(r.value == 999);
}

TEST(LoopClosedByReturnRunsItsBodyOnce)
{
    ProcSource procs[] = {PROC(1, loopClosedByReturnProc0)};
    uint32_t args[] = {1};
    ProgramResult r = runProgram(procs, 1, args);

    CHECK(!r.trapped);
    CHECK(r.value == 42);
}

TEST(LoopClosedByReturnRunsItsBodyOnceForAnyNonzero)
{
    ProcSource procs[] = {PROC(1, loopClosedByReturnProc0)};
    uint32_t args[] = {7};
    ProgramResult r = runProgram(procs, 1, args);

    CHECK(!r.trapped);
    CHECK(r.value == 42);
}

// ---- A genuine (non-degenerate) LOOP with real accumulation and a
// back-edge — sum(1..n).
static const Instr loopSumProc0[] = {
    LOAD(0), PUSH(),                             // k=1: counter := n
    CONST(0), PUSH(),                            // k=2: total := 0
    bare(Op::LOOP),
        LOAD(1),
    bare(Op::BLOCK_END),                          // testAccNonzero(counter)
        LOAD(2), opReg(Op::ADD, 1), STORE(2),      // total += counter
        LOAD(1), opImm(Op::SUB, 1), STORE(1),      // counter -= 1
    bare(Op::BLOCK_END),                          // back-edge
    LOAD(2), bare(Op::RETURN),
};

TEST(LoopSumToFour)
{
    ProcSource procs[] = {PROC(1, loopSumProc0)};
    uint32_t args[] = {4};
    ProgramResult r = runProgram(procs, 1, args);

    CHECK(!r.trapped);
    CHECK(r.value == 10);
}

TEST(LoopSumToZero)
{
    ProcSource procs[] = {PROC(1, loopSumProc0)};
    uint32_t args[] = {0};
    ProgramResult r = runProgram(procs, 1, args);

    CHECK(!r.trapped);
    CHECK(r.value == 0);
}

TEST(LoopSumToOne)
{
    ProcSource procs[] = {PROC(1, loopSumProc0)};
    uint32_t args[] = {1};
    ProgramResult r = runProgram(procs, 1, args);

    CHECK(!r.trapped);
    CHECK(r.value == 1);
}

// ---- BR_TABLE if/else fusion, non-last case closed via a bare RETURN,
// last case closes normally.
static const Instr ifElseProc0[] = {
    LOAD(0), opImm(Op::GT_U, 10), brTable(2),
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

// ---- BR_TABLE N>2, the shared jump-table helper.
static const Instr jumpTableProc0[] = {
    LOAD(0), brTable(4),
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

// ---- A comparison feeds further arithmetic — (n > 4) * 5.
static const Instr comparisonProc0[] = {LOAD(0), opImm(Op::GT_U, 4), opImm(Op::MUL, 5), bare(Op::RETURN)};

TEST(ComparisonFeedingArithmeticWhenTrue)
{
    ProcSource procs[] = {PROC(1, comparisonProc0)};
    uint32_t args[] = {6};
    ProgramResult r = runProgram(procs, 1, args);

    CHECK(!r.trapped);
    CHECK(r.value == 5);
}

TEST(ComparisonFeedingArithmeticWhenFalse)
{
    ProcSource procs[] = {PROC(1, comparisonProc0)};
    uint32_t args[] = {3};
    ProgramResult r = runProgram(procs, 1, args);

    CHECK(!r.trapped);
    CHECK(r.value == 0);
}

// ---- Unary ops, one procedure per op.
static const Instr negProc0[] = {LOAD(0), bare(Op::NEG), bare(Op::RETURN)};
static const Instr notProc0[] = {LOAD(0), bare(Op::NOT), bare(Op::RETURN)};
static const Instr clzProc0[] = {LOAD(0), bare(Op::CLZ), bare(Op::RETURN)};
static const Instr revbitsProc0[] = {LOAD(0), bare(Op::REVBITS), bare(Op::RETURN)};

TEST(UnaryNeg)
{
    ProcSource procs[] = {PROC(1, negProc0)};
    uint32_t args[] = {5};
    ProgramResult r = runProgram(procs, 1, args);

    CHECK(!r.trapped);
    CHECK(r.value == 0xFFFFFFFBu);
}

TEST(UnaryNot)
{
    ProcSource procs[] = {PROC(1, notProc0)};
    uint32_t args[] = {5};
    ProgramResult r = runProgram(procs, 1, args);

    CHECK(!r.trapped);
    CHECK(r.value == 0xFFFFFFFAu);
}

TEST(UnaryClzOfOne)
{
    ProcSource procs[] = {PROC(1, clzProc0)};
    uint32_t args[] = {1};
    ProgramResult r = runProgram(procs, 1, args);

    CHECK(!r.trapped);
    CHECK(r.value == 31);
}

TEST(UnaryClzOfZero)
{
    ProcSource procs[] = {PROC(1, clzProc0)};
    uint32_t args[] = {0};
    ProgramResult r = runProgram(procs, 1, args);

    CHECK(!r.trapped);
    CHECK(r.value == 32);
}

TEST(UnaryRevbits)
{
    ProcSource procs[] = {PROC(1, revbitsProc0)};
    uint32_t args[] = {1};
    ProgramResult r = runProgram(procs, 1, args);

    CHECK(!r.trapped);
    CHECK(r.value == 0x80000000u);
}

// ---- PEEK_PEEK two-op-in-place. 10 & 12 = 8.
static const Instr peekPeekProc0[] = {
    CONST(12), PUSH(),                 // k=0 = 12
    CONST(10),                          // acc = 10 (pending)
    opStack(Op::AND, Combo::PEEK_PEEK), // k0 := 10 & 12 = 8; acc poisoned
    POP(), bare(Op::RETURN),
};

TEST(PeekPeekTwoOperandInPlace)
{
    ProcSource procs[] = {PROC(0, peekPeekProc0)};
    ProgramResult r = runProgram(procs, 1, nullptr);

    CHECK(!r.trapped);
    CHECK(r.value == 8);
}

// ---- Branch-range guard forced into the long (invert-and-branch) form —
// case 0's own body is padded past the 240-byte safe span, so the dispatch
// guard itself can't be a bare short-form conditional branch.
static const Instr longGuardProc0[] = {
    LOAD(0), opImm(Op::GT_U, 100), brTable(2),
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

// ---- Regression for emitAddSubRsub with accShape and the IMM_ACC operand
// both compile-time immediates (CONST directly followed by an immediate
// arithmetic op), with the combined immediate too large for the imm3/imm8
// fast paths — materializing accShape into SCRATCH_REG and then the second
// immediate into that same register clobbers the first value, silently
// computing `k op k` instead of `accShape op k`.
static const Instr bothImmediateProc0[] = {CONST(5), opImm(Op::ADD, 1000), bare(Op::RETURN)};

TEST(BothImmediateOperandAliasing)
{
    ProcSource procs[] = {PROC(0, bothImmediateProc0)};
    ProgramResult r = runProgram(procs, 1, nullptr);

    CHECK(!r.trapped);
    CHECK(r.value == 1005);
}

// ---- The same aliasing bug class, but the operand register itself happens
// to be SCRATCH_REG, which happens for real whenever an out-of-window local
// gets reloaded via ldrSp(SCRATCH_REG, ...). argCount=1 starts with slot 0
// (the argument) in the window (tos=1); four PUSHes bring tos to 5, exactly
// the point (inWindow's own `tos - k <= WINDOW_SIZE`) where slot 0 gets
// evicted onto the real stack, so `opReg(ADD, 0)` must reload it through
// SCRATCH_REG right after CONST(100) leaves acc pending.
static const Instr scratchOperandProc0[] = {
    CONST(2), PUSH(), // slot1=2, tos=2
    CONST(3), PUSH(), // slot2=3, tos=3
    CONST(4), PUSH(), // slot3=4, tos=4
    CONST(5), PUSH(), // slot4=5, tos=5 — slot 0 (the argument) now spilled
    CONST(100),
    opReg(Op::ADD, 0),
    bare(Op::RETURN),
};

TEST(ScratchRegOperandAliasingWithOne)
{
    ProcSource procs[] = {PROC(1, scratchOperandProc0)};
    uint32_t args[] = {1};
    ProgramResult r = runProgram(procs, 1, args);

    CHECK(!r.trapped);
    CHECK(r.value == 101);
}

TEST(ScratchRegOperandAliasingWithFortyTwo)
{
    ProcSource procs[] = {PROC(1, scratchOperandProc0)};
    uint32_t args[] = {42};
    ProgramResult r = runProgram(procs, 1, args);

    CHECK(!r.trapped);
    CHECK(r.value == 142);
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
    LOAD(0), opImm(Op::LE_S, 5), brTable(2),      // if/else guarded directly by LE_S
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
    LOAD(0), opImm(Op::GE_U, 0x80), brTable(2),
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

// ---- The same bug's LOOP-body half — the fused condition closing LOOP's own
// condition sub-block has the identical gap. x = 7 (not 0/1) is forced to 0
// right inside the body instead of decremented normally, so the loop runs its
// body exactly once.
static const Instr accStateMergeInLoopProc0[] = {
    CONST(7), PUSH(),                        // slot0 = 7 (stale sentinel)
    CONST(0), PUSH(),                        // slot1 = probe target
    bare(Op::LOOP),
        LOAD(0), opImm(Op::GT_S, 0), bare(Op::BLOCK_END),
        STORE(1),
        CONST(0), STORE(0),
    bare(Op::BLOCK_END),
    LOAD(1),
    bare(Op::RETURN),
};

TEST(AccStateMergeInLoopBody)
{
    ProcSource procs[] = {PROC(0, accStateMergeInLoopProc0)};
    ProgramResult r = runProgram(procs, 1, nullptr);

    CHECK(!r.trapped);
    CHECK(r.value == 1);
}

// ---- Literal pooling, both routes at once. CONST's own hard-to-synthesize
// value pools, and so does the ADD's immediate operand (which reaches the
// pool through Combo::IMM_ACC rather than CONST), so this executes two
// PC-relative loads at different alignment parities.
static const Instr pooledLiteralProc0[] = {CONST(0x12345678), opImm(Op::ADD, 0x11111111), bare(Op::RETURN)};

TEST(PooledLiteralViaBothRoutes)
{
    ProcSource procs[] = {PROC(0, pooledLiteralProc0)};
    ProgramResult r = runProgram(procs, 1, nullptr);

    CHECK(!r.trapped);
    CHECK(r.value == 0x23456789u);
}

// ---- A pooled load whose pool is flushed mid-procedure, with the flush's
// branch-around actually executed. BR_TABLE N>2 forces the flush (its jump
// table's raw halfwords must not land in a later scan window), so the pool
// lands in the middle of the code and control has to jump over it to reach
// the dispatch. The pooled value is read back afterwards from a local,
// proving both the load and the jump-around worked.
static const Instr pooledFlushProc0[] = {
    CONST(0xDEADBEEF), PUSH(), // pooled — the chunk is open across the BR_TABLE
    CONST(1), brTable(3),       // forces the flush, mid-code, before the table
        CONST(100), bare(Op::BLOCK_END),
        CONST(200), bare(Op::BLOCK_END),
        CONST(300), bare(Op::BLOCK_END),
    LOAD(0),
    bare(Op::RETURN),
};

TEST(PooledLiteralFlushedMidProcedure)
{
    ProcSource procs[] = {PROC(0, pooledFlushProc0)};
    ProgramResult r = runProgram(procs, 1, nullptr);

    CHECK(!r.trapped);
    CHECK(r.value == 0xDEADBEEFu);
}

// ---- A pooled load in a procedure that does *not* start at the arena base.
// proc0 compiles to an odd number of halfwords (38 bytes, i.e. 2 mod 4), so
// without Runtime::allocate rounding its reservation up proc1 would land 2
// bytes off a word boundary — and every Align(pc,4)-based literal offset in
// it, resolved procedure-relative at translation time, would then read 2
// bytes away from its own pool word. This is the test that actually fails if
// that rounding is dropped.
static const Instr unalignedPoolProc0[] = {CONST(5), call(1), opImm(Op::ADD, 1), bare(Op::RETURN)};
static const Instr unalignedPoolProc1[] = {LOAD(0), opImm(Op::XOR, 0x0F0F0F0F), bare(Op::RETURN)};

TEST(PooledLiteralPastAnOddSizedProcedure)
{
    ProcSource procs[] = {PROC(0, unalignedPoolProc0), PROC(1, unalignedPoolProc1)};
    ProgramResult r = runProgram(procs, 2, nullptr);

    CHECK(!r.trapped);
    CHECK(r.value == 0x0F0F0F0Bu); // 5 ^ 0x0F0F0F0F = 0x0F0F0F0A, + 1
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

// ---- Nested LOOP-in-LOOP, sum of triangular numbers
// (sum_{i=1..n} sum_{j=1..i} j). The only 2-level LOOP nesting here
// (maxSpanBytes/translateLoop recursion at depth 2 — every other LOOP test
// nests one level only). All four working locals (k1..k4) are PUSHed once,
// ahead of the outer LOOP, and only ever STOREd inside either loop body — tos
// stays fixed at 5 (k0 arg + k1..k4) across both loops' own back-edges, which
// also spills k0 out of the window for free (WINDOW_SIZE is 4). Body lives in
// corpus_programs.h, shared with test/tools/dump_corpus.cpp.
TEST(NestedLoopsWithThree)
{
    ProcSource procs[] = {PROC(1, corpusNestedLoopProc0)};
    uint32_t args[] = {3};
    ProgramResult r = runProgram(procs, 1, args);

    CHECK(!r.trapped);
    CHECK(r.value == 10); // 3+2+1 via 6+3+1
}

TEST(NestedLoopsWithOne)
{
    ProcSource procs[] = {PROC(1, corpusNestedLoopProc0)};
    uint32_t args[] = {1};
    ProgramResult r = runProgram(procs, 1, args);

    CHECK(!r.trapped);
    CHECK(r.value == 1);
}

TEST(NestedLoopsWithZero)
{
    ProcSource procs[] = {PROC(1, corpusNestedLoopProc0)};
    uint32_t args[] = {0};
    ProgramResult r = runProgram(procs, 1, args);

    CHECK(!r.trapped);
    CHECK(r.value == 0);
}

// ---- BR_TABLE nested inside a LOOP body. Each iteration dispatches on
// counter&1 (even -> total += counter*10, odd -> total += counter), both
// cases closed via BLOCK_END so control rejoins the loop's own decrement
// before the back-edge — the interaction between fused-branch dispatch and a
// live loop back-edge, distinct from AccStateMergeInLoopBody's fused *loop
// condition* itself. Body lives in corpus_programs.h.
TEST(BrTableInLoopBodyWithFour)
{
    ProcSource procs[] = {PROC(1, corpusBrTableInLoopProc0)};
    uint32_t args[] = {4};
    ProgramResult r = runProgram(procs, 1, args);

    CHECK(!r.trapped);
    CHECK(r.value == 64);
}

TEST(BrTableInLoopBodyWithFive)
{
    ProcSource procs[] = {PROC(1, corpusBrTableInLoopProc0)};
    uint32_t args[] = {5};
    ProgramResult r = runProgram(procs, 1, args);

    CHECK(!r.trapped);
    CHECK(r.value == 69);
}

TEST(BrTableInLoopBodyWithZero)
{
    ProcSource procs[] = {PROC(1, corpusBrTableInLoopProc0)};
    uint32_t args[] = {0};
    ProgramResult r = runProgram(procs, 1, args);

    CHECK(!r.trapped);
    CHECK(r.value == 0);
}

// ---- LOOP nested inside a BR_TABLE case — the mirror image of
// BrTableInLoopBody*. Selector and n both travel packed into this program's
// single argument: selector in bits[15:8], n in bits[7:0]. Case 0 runs a full
// sum(1..n) LOOP using two extra PUSHed locals, then POPs them off again
// before the case's own BLOCK_END so tos returns to its pre-brTable value (1),
// matching case 1 (which never touches tos) — POP() mirrors PUSH() by loading
// the popped slot's own value back into acc, so total is pushed *before*
// counter: the first POP discards counter's spent (zero) value, and the second
// POP is the one that lands the real result in acc, which the case then STOREs
// to the result slot k1 — acc itself cannot cross a BR_TABLE's merge point
// (isa-core.md §8.7), so a value-producing dispatch delivers through a slot.
// Body lives in corpus_programs.h.
TEST(LoopInBrTableCaseZero)
{
    ProcSource procs[] = {PROC(1, corpusLoopInBrTableProc0)};
    uint32_t args[] = {4}; // selector 0, n 4
    ProgramResult r = runProgram(procs, 1, args);

    CHECK(!r.trapped);
    CHECK(r.value == 10);
}

TEST(LoopInBrTableCaseOne)
{
    ProcSource procs[] = {PROC(1, corpusLoopInBrTableProc0)};
    uint32_t args[] = {260}; // selector 1, n 4
    ProgramResult r = runProgram(procs, 1, args);

    CHECK(!r.trapped);
    CHECK(r.value == 12);
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

// ---- Deep operand stack, 24 live locals. Everything past k3 is
// out-of-window (WINDOW_SIZE=4), so the chain of ADDs reload-addresses 20
// spilled slots in one procedure — several times deeper than
// MoreStackArgumentsThanTheWindowHolds' 6 stack args. Body lives in
// corpus_programs.h.
TEST(DeepOperandStack)
{
    ProcSource procs[] = {PROC(0, corpusDeepStackProc0)};
    ProgramResult r = runProgram(procs, 1, nullptr);

    CHECK(!r.trapped);
    CHECK(r.value == 300); // 1+2+...+24
}

// ---- Acc-fold thrash inside a loop. Per iteration:
// total := (total + 7) & 0xF -- an operand-fold-eligible ADD immediately
// followed by a never-folds AND, repeated across several loop iterations
// (RotationOperandFold/RotationDestinationFold/PeekPeekTwoOperandInPlace
// exercise fold/no-fold transitions exactly once; this repeats it every
// back-edge).
static const Instr accFoldThrashProc0[] = {
    LOAD(0), PUSH(),  // k1 = counter := n
    CONST(0), PUSH(), // k2 = total := 0
    bare(Op::LOOP),
        LOAD(1),
    bare(Op::BLOCK_END), // while(counter != 0)
        LOAD(2), opImm(Op::ADD, 7), opImm(Op::AND, 0xF), STORE(2), // total := (total+7)&0xF
        LOAD(1), opImm(Op::SUB, 1), STORE(1),
    bare(Op::BLOCK_END), // back-edge
    LOAD(2), bare(Op::RETURN),
};

TEST(AccFoldThrashInsideALoop)
{
    ProcSource procs[] = {PROC(1, accFoldThrashProc0)};
    uint32_t args[] = {5};
    ProgramResult r = runProgram(procs, 1, args);

    CHECK(!r.trapped);
    CHECK(r.value == 3); // 0->7->14->5->12->3
}

// ---- NEG/NOT consuming an out-of-window (spilled) operand. Mirrors
// UnaryNeg/UnaryNot, but with 4 PUSHes ahead of the arg so k0 is spilled by
// the time LOAD(0) reloads it, immediately followed by the unary op -- the
// exact shape emitUnary's src-parameter path needs (reading the reload's own
// destination register directly instead of forcing a flush through ACC_REG
// first). No other test here covers a spilled operand for a unary op.
static const Instr spilledNegProc0[] = {
    CONST(1), PUSH(), CONST(2), PUSH(), CONST(3), PUSH(), CONST(4), PUSH(),
    LOAD(0), bare(Op::NEG), bare(Op::RETURN),
};
static const Instr spilledNotProc0[] = {
    CONST(1), PUSH(), CONST(2), PUSH(), CONST(3), PUSH(), CONST(4), PUSH(),
    LOAD(0), bare(Op::NOT), bare(Op::RETURN),
};

TEST(UnaryNegOnASpilledOperand)
{
    ProcSource procs[] = {PROC(1, spilledNegProc0)};
    uint32_t args[] = {5};
    ProgramResult r = runProgram(procs, 1, args);

    CHECK(!r.trapped);
    CHECK(r.value == 0xFFFFFFFBu);
}

TEST(UnaryNotOnASpilledOperand)
{
    ProcSource procs[] = {PROC(1, spilledNotProc0)};
    uint32_t args[] = {5};
    ProgramResult r = runProgram(procs, 1, args);

    CHECK(!r.trapped);
    CHECK(r.value == 0xFFFFFFFAu);
}

// ---- LOOP back-edge forced into the long-branch form. Padded with 20
// bare(Op::NOT)s inside the loop body (mirroring LongFormBranchGuard*'s
// technique, but applied to translateLoop's own back-edge rather than an
// if/else guard -- 21*ORDINARY_MAX_BYTES(16) = 336 >
// SAFE_COND_BRANCH_SPAN(240)). Counts down to 0 regardless of padding; the
// interesting part is that it compiles and runs at all.
static const Instr longBackEdgeProc0[] = {
    LOAD(0), PUSH(), // k1 = counter := n
    bare(Op::LOOP),
        LOAD(1),
    bare(Op::BLOCK_END), // while(counter != 0)
        bare(Op::NOT), bare(Op::NOT), bare(Op::NOT), bare(Op::NOT), bare(Op::NOT),
        bare(Op::NOT), bare(Op::NOT), bare(Op::NOT), bare(Op::NOT), bare(Op::NOT),
        bare(Op::NOT), bare(Op::NOT), bare(Op::NOT), bare(Op::NOT), bare(Op::NOT),
        bare(Op::NOT), bare(Op::NOT), bare(Op::NOT), bare(Op::NOT), bare(Op::NOT),
        LOAD(1), opImm(Op::SUB, 1), STORE(1),
    bare(Op::BLOCK_END), // back-edge, forced into the long form
    LOAD(1), bare(Op::RETURN),
};

TEST(LongFormLoopBackEdgeWithOne)
{
    ProcSource procs[] = {PROC(1, longBackEdgeProc0)};
    uint32_t args[] = {1};
    ProgramResult r = runProgram(procs, 1, args);

    CHECK(!r.trapped);
    CHECK(r.value == 0);
}

TEST(LongFormLoopBackEdgeWithFifty)
{
    ProcSource procs[] = {PROC(1, longBackEdgeProc0)};
    uint32_t args[] = {50};
    ProgramResult r = runProgram(procs, 1, args);

    CHECK(!r.trapped);
    CHECK(r.value == 0);
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

// ---- A bytecode TRAP really unwinds (isa-core.md §4.5/§9). This is the
// four-instruction program fuzz/qemu_exec minimized the original
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

// ---- Multi-argument ENTRY procedures.
//
// Everything above reaches an out-of-window argument through proc1 or deeper,
// never through proc0, because until enterDispatch learned to marshal an
// argument vector there was no way to give the entry procedure more than the
// single acc-borne word. Both halves of that gap are covered here: 2..4
// arguments (window registers enterDispatch never used to initialize, so they
// arrived holding the caller's r8-r11) and 5+ (where the epilogue also
// reclaimed a frame nobody had pushed, landing .Lresume on a shifted sp — a
// deterministic hang, not a wrong answer).
//
// Bodies pack their arguments into nibbles rather than summing them: a sum is
// invariant under any permutation of the window, which is precisely the error
// class most likely here. args {1,2,3,...} therefore expect 0x123..., and any
// swapped register or mis-ordered spill slot changes the result.

// Nibble-pack k = 0..N-1: acc = arg0, then (acc << 4) | arg_k.
#define PACK_ARG(k) opImm(Op::SHL, 4), opReg(Op::OR, k)

// Two arguments — entirely in-window. arg0 lands in physReg(0) = r7, which
// enterDispatch had no way to write at all.
static const Instr entryArgs2Proc0[] = {LOAD(0), PACK_ARG(1), bare(Op::RETURN)};

TEST(EntryWithTwoArguments)
{
    ProcSource procs[] = {PROC(2, entryArgs2Proc0)};
    uint32_t args[] = {1, 2};
    ProgramResult r = runProgram(procs, 1, args);

    CHECK(!r.trapped);
    CHECK(r.value == 0x12u);
}

// Four arguments — exactly fills the window, and exactly fillCalleeArgs's
// WINDOW_SIZE-1 cap (r7/r6/r5 supplied by the caller, r4 from acc). Still
// nothing spilled.
static const Instr entryArgs4Proc0[] = {LOAD(0), PACK_ARG(1), PACK_ARG(2), PACK_ARG(3), bare(Op::RETURN)};

TEST(EntryWithFourArguments)
{
    ProcSource procs[] = {PROC(4, entryArgs4Proc0)};
    uint32_t args[] = {1, 2, 3, 4};
    ProgramResult r = runProgram(procs, 1, args);

    CHECK(!r.trapped);
    CHECK(r.value == 0x1234u);
}

// Five arguments — one spilled word, read through spillOffset(0) == 0. Leaf,
// so the whole frame comes back via discardWindow's single ADD sp.
static const Instr entryArgs5Proc0[] = {
    LOAD(0), PACK_ARG(1), PACK_ARG(2), PACK_ARG(3), PACK_ARG(4), bare(Op::RETURN),
};

TEST(EntryWithFiveArguments)
{
    ProcSource procs[] = {PROC(5, entryArgs5Proc0)};
    uint32_t args[] = {1, 2, 3, 4, 5};
    ProgramResult r = runProgram(procs, 1, args);

    CHECK(!r.trapped);
    CHECK(r.value == 0x12345u);
}

// Six arguments — TWO spilled words, the smallest shape in which their order
// is observable at all (five spills exactly one, so a reversed push loop looks
// identical there).
static const Instr entryArgs6Proc0[] = {
    LOAD(0), PACK_ARG(1), PACK_ARG(2), PACK_ARG(3), PACK_ARG(4), PACK_ARG(5), bare(Op::RETURN),
};

TEST(EntryWithSixArguments)
{
    ProcSource procs[] = {PROC(6, entryArgs6Proc0)};
    uint32_t args[] = {1, 2, 3, 4, 5, 6};
    ProgramResult r = runProgram(procs, 1, args);

    CHECK(!r.trapped);
    CHECK(r.value == 0x123456u);
}

// Eight arguments — the post-wrap window phase (slots 4..6 in r7/r6/r5, slot
// 7 from acc into physReg(7) = r4) with four spilled words.
static const Instr entryArgs8Proc0[] = {
    LOAD(0), PACK_ARG(1), PACK_ARG(2), PACK_ARG(3),
    PACK_ARG(4), PACK_ARG(5), PACK_ARG(6), PACK_ARG(7), bare(Op::RETURN),
};

TEST(EntryWithEightArguments)
{
    ProcSource procs[] = {PROC(8, entryArgs8Proc0)};
    uint32_t args[] = {1, 2, 3, 4, 5, 6, 7, 8};
    ProgramResult r = runProgram(procs, 1, args);

    CHECK(!r.trapped);
    CHECK(r.value == 0x12345678u);
}

// Six arguments AND a nested CALL, so the entry procedure is savesLR. Its own
// push{lr} lands above the words enterDispatch placed, which is exactly the +4
// shift Window::spillOffset applies to k < initialSpilledCount — and the
// return goes through returnHelperFromStackReclaim with r2 = 8, on an *entry*
// frame. This is the shape that hangs deterministically without the fix;
// SavesLrReturnWithOutOfWindowArgs/TwoLevelSavesLrChain only ever reach it via
// proc1.
static const Instr entryArgs6CallProc0[] = {
    LOAD(0), PACK_ARG(1), PACK_ARG(2), PACK_ARG(3), PACK_ARG(4), PACK_ARG(5),
    PUSH(),           // keep the packed value while the callee runs
    CONST(0),
    call(1),
    opReg(Op::ADD, 6), // += the packed value
    bare(Op::RETURN),
};
static const Instr entryArgs6CallProc1[] = {CONST(1000), bare(Op::RETURN)};

TEST(EntryWithSixArgumentsAndANestedCall)
{
    ProcSource procs[] = {PROC(6, entryArgs6CallProc0), PROC(0, entryArgs6CallProc1)};
    uint32_t args[] = {1, 2, 3, 4, 5, 6};
    ProgramResult r = runProgram(procs, 2, args);

    CHECK(!r.trapped);
    CHECK(r.value == 0x123456u + 1000u);
}

// Six arguments, body TRAPs. trapHelper restores sp from runtime->savedSp,
// which is only correct if savedSp was captured *before* enterDispatch pushed
// the arguments. Captured after, .Lresume would pop those words as the
// caller's r8-r11 and return to a garbage pc — a hang, so this reports a wrong
// outcome rather than a wrong value.
static const Instr entryArgs6TrapProc0[] = {
    LOAD(0), PACK_ARG(1), PACK_ARG(2), PACK_ARG(3), PACK_ARG(4), PACK_ARG(5),
    trapInstr(0x123456),
};

TEST(EntryWithSixArgumentsThatTraps)
{
    ProcSource procs[] = {PROC(6, entryArgs6TrapProc0)};
    uint32_t args[] = {1, 2, 3, 4, 5, 6};
    ProgramResult r = runProgram(procs, 1, args);

    CHECK(r.trapped == LANDING_TRAP);
    CHECK(r.value == 0x123456u);
}

// Six-argument entry whose callee traps — the same unwind one level down,
// with the entry procedure's own out-of-window arguments live below the frame
// being discarded.
static const Instr entryArgs6CalleeTrapProc0[] = {
    LOAD(0), PACK_ARG(1), PACK_ARG(2), PACK_ARG(3), PACK_ARG(4), PACK_ARG(5),
    call(1), bare(Op::RETURN),
};
static const Instr entryArgs6CalleeTrapProc1[] = {trapInstr(4242)};

TEST(EntryWithSixArgumentsWhoseCalleeTraps)
{
    ProcSource procs[] = {PROC(6, entryArgs6CalleeTrapProc0), PROC(0, entryArgs6CalleeTrapProc1)};
    uint32_t args[] = {1, 2, 3, 4, 5, 6};
    ProgramResult r = runProgram(procs, 2, args);

    CHECK(r.trapped == LANDING_TRAP);
    CHECK(r.value == 4242);
}

#undef PACK_ARG
