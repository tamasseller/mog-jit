// LOOP: exits, back-edges, and the accState the condition sub-block leaves behind.

#include <cstdint>

#include "instr.h"
#include "run_program.h"
#include "Test.h"

using namespace jitc;

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
