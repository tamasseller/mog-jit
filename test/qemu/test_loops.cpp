// Loops: exits, back-edges, both openers, and the accState the condition
// sub-block leaves behind. Body block first, condition second (isa-core.md
// §7.2), so the back-edge spans the condition and the entry branch the body.

#include <cstdint>

#include "instr.h"
#include "run_program.h"
#include "Test.h"

using namespace jitc;

// ---- LOOP body closed by a bare terminator, not BLOCK_END.
static const Instr loopClosedByReturnProc0[] = {
    bare(Op::LOOP_PRE),
    CONST(42), bare(Op::RETURN),  // body — bare terminator closes it
    bare(Op::BLOCK_END),          // condition sub-block is empty — tests the incoming arg
    CONST(999), bare(Op::RETURN), // reached only via the cond-false exit
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
    bare(Op::LOOP_PRE),
        LOAD(2), opReg(Op::ADD, 1), STORE(2),      // total += counter
        LOAD(1), opImm(Op::SUB, 1), STORE(1),      // counter -= 1
    bare(Op::BLOCK_END),
        LOAD(1),
    bare(Op::BLOCK_END),                          // back-edge while counter != 0
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

// ---- The back-edge forced into the long-branch form. It runs from the end
// of the condition block to the start of the body, so it is the *condition*
// that has to be padded now — 20 bare(Op::NOT)s, an even number, so acc
// still reads back as the counter (21*ORDINARY_MAX_BYTES(16) = 336 >
// SAFE_COND_BRANCH_SPAN(240)). Counts down to 0 regardless of the padding;
// the interesting part is that it compiles and runs at all.
static const Instr longBackEdgeProc0[] = {
    LOAD(0), PUSH(), // k1 = counter := n
    bare(Op::LOOP_PRE),
        LOAD(1), opImm(Op::SUB, 1), STORE(1),
    bare(Op::BLOCK_END),
        LOAD(1),
        bare(Op::NOT), bare(Op::NOT), bare(Op::NOT), bare(Op::NOT), bare(Op::NOT),
        bare(Op::NOT), bare(Op::NOT), bare(Op::NOT), bare(Op::NOT), bare(Op::NOT),
        bare(Op::NOT), bare(Op::NOT), bare(Op::NOT), bare(Op::NOT), bare(Op::NOT),
        bare(Op::NOT), bare(Op::NOT), bare(Op::NOT), bare(Op::NOT), bare(Op::NOT),
    bare(Op::BLOCK_END), // back-edge, forced into the long form
    LOAD(1), bare(Op::RETURN),
};

// ---- The other half of the split budget: a body long enough that the
// entry branch has to carry it. Same identity padding, same answer.
static const Instr longEntryBranchProc0[] = {
    LOAD(0), PUSH(), // k1 = counter := n
    bare(Op::LOOP_PRE),
        LOAD(1),
        bare(Op::NOT), bare(Op::NOT), bare(Op::NOT), bare(Op::NOT), bare(Op::NOT),
        bare(Op::NOT), bare(Op::NOT), bare(Op::NOT), bare(Op::NOT), bare(Op::NOT),
        bare(Op::NOT), bare(Op::NOT), bare(Op::NOT), bare(Op::NOT), bare(Op::NOT),
        bare(Op::NOT), bare(Op::NOT), bare(Op::NOT), bare(Op::NOT), bare(Op::NOT),
        opImm(Op::SUB, 1), STORE(1),
    bare(Op::BLOCK_END),
        LOAD(1),
    bare(Op::BLOCK_END),
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

TEST(LongFormLoopEntryBranchWithOne)
{
    ProcSource procs[] = {PROC(1, longEntryBranchProc0)};
    uint32_t args[] = {1};
    ProgramResult r = runProgram(procs, 1, args);

    CHECK(!r.trapped);
    CHECK(r.value == 0);
}

TEST(LongFormLoopEntryBranchWithFifty)
{
    ProcSource procs[] = {PROC(1, longEntryBranchProc0)};
    uint32_t args[] = {50};
    ProgramResult r = runProgram(procs, 1, args);

    CHECK(!r.trapped);
    CHECK(r.value == 0);
}

// ---- LOOP_POST against its LOOP_PRE twin: identical body and condition,
// so the only difference is where the construct is entered. Counting
// iterations is what makes that visible — at n = 0 the post-test form runs
// its body once and the pre-test form not at all.
#define ITERATION_COUNTER_BODY(opener)                       \
    LOAD(0), PUSH(),                  /* k1 = n           */ \
    CONST(0), PUSH(),                 /* k2 = iterations  */ \
    CONST(0), PUSH(),                 /* k3 = i           */ \
    bare(opener),                                            \
        LOAD(2), opImm(Op::ADD, 1), STORE(2),                \
        LOAD(3), opImm(Op::ADD, 1), STORE(3),                \
    bare(Op::BLOCK_END),                                     \
        LOAD(3), opReg(Op::LT_U, 1),  /* while i < n      */ \
    bare(Op::BLOCK_END),                                     \
    LOAD(2), bare(Op::RETURN)

static const Instr postTestCountProc0[] = {ITERATION_COUNTER_BODY(Op::LOOP_POST)};
static const Instr preTestCountProc0[] = {ITERATION_COUNTER_BODY(Op::LOOP_PRE)};

static uint32_t iterations(const Instr *body, uint32_t bodyLen, uint32_t n)
{
    ProcSource procs[] = {ProcSource{1, body, bodyLen}};
    uint32_t args[] = {n};
    ProgramResult r = runProgram(procs, 1, args);

    CHECK(!r.trapped);
    return r.value;
}

#define POST_TEST_COUNT(n) iterations(postTestCountProc0, sizeof(postTestCountProc0) / sizeof(Instr), (n))
#define PRE_TEST_COUNT(n)  iterations(preTestCountProc0, sizeof(preTestCountProc0) / sizeof(Instr), (n))

TEST(PostTestLoopRunsItsBodyOnceWhenTheConditionIsFalseAtEntry)
{
    CHECK(POST_TEST_COUNT(0) == 1);
    CHECK(PRE_TEST_COUNT(0) == 0);
}

TEST(PostTestLoopAgreesWithThePreTestFormOnceTheBodyRunsAtAll)
{
    CHECK(POST_TEST_COUNT(1) == 1);
    CHECK(PRE_TEST_COUNT(1) == 1);

    CHECK(POST_TEST_COUNT(4) == 4);
    CHECK(PRE_TEST_COUNT(4) == 4);
}
