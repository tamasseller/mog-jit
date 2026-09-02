// Two-level block nesting, all three combinations — LOOP in LOOP,
// BR_TABLE in a LOOP body, LOOP in a BR_TABLE case. Bodies live in
// corpus_programs.h, shared with the fuzz seed corpus.

#include <cstdint>

#include "instr.h"
#include "corpus_programs.h"
#include "run_program.h"
#include "Test.h"

using namespace jitc;

// ---- Nested LOOP-in-LOOP, sum of triangular numbers
// (sum_{i=1..n} sum_{j=1..i} j). The only 2-level LOOP nesting in the suite
// (maxSpanBytes/translateLoop recursion at depth 2 — test_loops.cpp's own
// LOOP TESTs all nest one level only). All four working locals (k1..k4) are PUSHed once,
// ahead of the outer LOOP, and only ever STOREd inside either loop body — tos
// stays fixed at 5 (k0 arg + k1..k4) across both loops' own back-edges, which
// also spills k0 out of the window for free (WINDOW_SIZE is 4). Body lives in
// corpus_programs.h, shared with fuzz/dump_seeds.cpp.
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
// live loop back-edge, distinct from a fused *loop condition* itself. Body
// lives in corpus_programs.h.
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
// sum(1..n) LOOP using two extra PUSHed locals; the case's own BLOCK_END is
// what returns tos to its pre-brTable value (1), matching case 1 (which never
// touches tos). The running total is what the case LOADs back and STOREs to
// the result slot k1 — one case ends in a LOOP, after which acc is dead
// (isa-core.md §8.7), so this dispatch delivers through a slot.
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
