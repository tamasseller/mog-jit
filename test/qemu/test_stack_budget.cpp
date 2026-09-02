// The up-front stack budget: what Executor::onStack/split reserve before a
// program runs, and where the boundary between accepting and refusing one
// actually sits.

#include <cstdint>

#include "ext.h"
#include "instr.h"
#include "encode_instr.h"
#include "measure_proc.h"
#include "runtime.h"
#include "executor.h"
#include "dispatch_abi.h"
#include "Test.h"
#include "semihosting_output.h"

using namespace jitc;

extern "C" uint8_t __bss_end;

static constexpr uint32_t GENEROUS_ARENA = 400;
static constexpr uint32_t STACK_SLACK_ABOVE_BSS = 128;

static uint32_t currentSp()
{
    register uint32_t sp asm("sp");
    return sp;
}

static uint32_t stackLimitAboveBss()
{
    return (uint32_t)(uintptr_t)&__bss_end + STACK_SLACK_ABOVE_BSS;
}

TEST(OnStackGenerousSucceeds)
{
    const Instr proc0Body[] = {CONST(37), call(1), bare(Op::RETURN)};
    const Instr proc1Body[] = {LOAD(0), opImm(Op::ADD, 5), bare(Op::RETURN)};
    ProcSource procs[] = {{0, proc0Body, 3}, {1, proc1Body, 3}};
    uint8_t bytes[32];
    uint32_t len = encodeJitProgram(/*maxCallDepth=*/1, /*totalDepth=*/1, procs, 2, bytes, sizeof(bytes));

    uint32_t stackLimit = stackLimitAboveBss();
    ProgramResult r = Executor::onStack(stackLimit, /*interruptReserve=*/0).run(bcMapped(bytes), len, nullptr, 0);

    if(r.trapped)
    {
        writeHexTrap(r.value);
    }
    CHECK(!r.trapped);
    CHECK(r.value == 42);
}

TEST(SplitThreeDeepCallChainSucceeds)
{
    const Instr proc0Body[] = {CONST(5), call(1), bare(Op::RETURN)};
    const Instr proc1Body[] = {LOAD(0), call(2), opImm(Op::ADD, 1), bare(Op::RETURN)};
    const Instr proc2Body[] = {LOAD(0), opImm(Op::ADD, 100), bare(Op::RETURN)};
    ProcSource procs[] = {{0, proc0Body, 3}, {1, proc1Body, 4}, {1, proc2Body, 3}};
    uint8_t bytes[48];
    uint32_t len = encodeJitProgram(/*maxCallDepth=*/2, /*totalDepth=*/2, procs, 3, bytes, sizeof(bytes));

    static uint8_t arena[GENEROUS_ARENA];
    uint32_t stackLimit = stackLimitAboveBss();
    ProgramResult r = Executor::split((uint32_t)(uintptr_t)arena, GENEROUS_ARENA, stackLimit, /*interruptReserve=*/0)
        .run(bcMapped(bytes), len, nullptr, 0);

    if(r.trapped)
    {
        writeHexTrap(r.value);
    }
    CHECK(!r.trapped);
    CHECK(r.value == 106);
}

TEST(OnStackRejectsBeforeTouchingAnything)
{
    const Instr proc0Body[] = {CONST(37), call(1), bare(Op::RETURN)};
    const Instr proc1Body[] = {LOAD(0), opImm(Op::ADD, 5), bare(Op::RETURN)};
    ProcSource procs[] = {{0, proc0Body, 3}, {1, proc1Body, 3}};
    uint8_t bytes[32];
    uint32_t len = encodeJitProgram(/*maxCallDepth=*/1, /*totalDepth=*/1, procs, 2, bytes, sizeof(bytes));

    uint32_t stackLimit = currentSp(); // measured before this callee's own prologue — strictly higher than sp once inside it
    ProgramResult r = Executor::onStack(stackLimit, /*interruptReserve=*/0).run(bcMapped(bytes), len, nullptr, 0);

    if(r.trapped)
    {
        writeHexTrap(r.value);
    }
    else
    {
        writeHexResult(r.value);
    }
    CHECK(r.trapped);
    CHECK(r.value == RESOURCE_EXHAUSTED_STACK_BUDGET);
}

// requiredStackBytes, reproduced from executor.cpp's own static
// function of the same name (not exported — computed here from the same
// public Runtime::storageBytesFor and dispatch_abi.h constants it uses) so
// the two boundary TESTs below can derive stackLimit from the exact
// formula the real upfront check applies, rather than the deliberately
// generous stackLimitAboveBss() every other Executor::onStack TEST here
// relies on. run_program.h's own envelope is deliberately too slack to
// reject anything, so these are the only TESTs that push real,
// hand-derived max_call_depth/total_depth values up against the computed
// floor.
static uint32_t requiredStackBytesFor(uint32_t procCount, uint32_t totalDepth, uint32_t maxCallDepth)
{
    return Runtime::storageBytesFor(procCount)
         + totalDepth * 4
         + maxCallDepth * CALL_RECORD_BYTES
         + ENTER_DISPATCH_FIXED_BYTES
         + EXECUTOR_RUN_FRAME_BYTES
         + TRANSLATOR_ENTRY_WORST_CASE_BYTES;
}

// A margin comfortably larger than the handful of stack frames between
// where this TEST measures currentSp() and where Executor::run's own
// codeLimitFor() re-measures it a few calls deeper — large enough to
// absorb that gap reliably, small enough (versus TRANSLATOR_ENTRY_WORST_
// CASE_BYTES=512 alone) that the boundary is still meaningfully tight
// rather than arbitrarily generous.
static constexpr uint32_t BOUNDARY_SLACK = 256;

// Executor::onStack does not take an arena size — the arena is whatever
// sits between stackLimit and the code limit the formula above computes. So
// what these TESTs choose is how much to leave there: enough for the compiled
// chain plus the translator's own live per-level margin, which is checked
// against the arena's top rather than reserved up front.
static constexpr uint32_t ARENA_ALLOWANCE = GENEROUS_ARENA + BOUNDARY_SLACK;

TEST(OnStackAcceptsAtComputedBudgetBoundary)
{
    // The same 3-deep chain as SplitThreeDeepCallChainSucceeds
    // (maxCallDepth=2, totalDepth=2, procCount=3), with stackLimit set
    // just under the real computed floor.
    const Instr proc0Body[] = {CONST(5), call(1), bare(Op::RETURN)};
    const Instr proc1Body[] = {LOAD(0), call(2), opImm(Op::ADD, 1), bare(Op::RETURN)};
    const Instr proc2Body[] = {LOAD(0), opImm(Op::ADD, 100), bare(Op::RETURN)};
    ProcSource procs[] = {{0, proc0Body, 3}, {1, proc1Body, 4}, {1, proc2Body, 3}};
    uint8_t bytes[48];
    uint32_t maxCallDepth = 2, totalDepth = 2, procCount = 3;
    uint32_t len = encodeJitProgram(maxCallDepth, totalDepth, procs, procCount, bytes, sizeof(bytes));

    uint32_t stackLimit = currentSp() - requiredStackBytesFor(procCount, totalDepth, maxCallDepth) - ARENA_ALLOWANCE;

    ProgramResult r = Executor::onStack(stackLimit, /*interruptReserve=*/0).run(bcMapped(bytes), len, nullptr, 0);

    if(r.trapped)
    {
        writeHexTrap(r.value);
    }
    CHECK(!r.trapped);
    CHECK(r.value == 106);
}

TEST(OnStackRejectsJustAboveComputedBudget)
{
    // Same program and formula as above, but stackLimit sits just above
    // the computed floor instead of just below it — the upfront check
    // should reject based on the real arithmetic, not the trivial
    // stackLimit==currentSp() case OnStackRejectsBeforeTouchingAnything
    // already covers.
    const Instr proc0Body[] = {CONST(5), call(1), bare(Op::RETURN)};
    const Instr proc1Body[] = {LOAD(0), call(2), opImm(Op::ADD, 1), bare(Op::RETURN)};
    const Instr proc2Body[] = {LOAD(0), opImm(Op::ADD, 100), bare(Op::RETURN)};
    ProcSource procs[] = {{0, proc0Body, 3}, {1, proc1Body, 4}, {1, proc2Body, 3}};
    uint8_t bytes[48];
    uint32_t maxCallDepth = 2, totalDepth = 2, procCount = 3;
    uint32_t len = encodeJitProgram(maxCallDepth, totalDepth, procs, procCount, bytes, sizeof(bytes));

    // No arena term: the reservation itself is the whole floor now, so
    // BOUNDARY_SLACK above it lands inside what the program has already
    // claimed rather than merely inside the arena it would have got.
    uint32_t stackLimit = currentSp() - requiredStackBytesFor(procCount, totalDepth, maxCallDepth) + BOUNDARY_SLACK;

    ProgramResult r = Executor::onStack(stackLimit, /*interruptReserve=*/0).run(bcMapped(bytes), len, nullptr, 0);

    if(r.trapped)
    {
        writeHexTrap(r.value);
    }
    else
    {
        writeHexResult(r.value);
    }
    CHECK(r.trapped);
    CHECK(r.value == RESOURCE_EXHAUSTED_STACK_BUDGET);
}

TEST(OnStackSucceedsWithTheArenaTrimmedToWhatTheChainNeeds)
{
    // Runtime::pushStackFloor() (runtime.cpp): Executor::onStack
    // anchors the code arena's own base at stackLimit itself, so
    // arenaCursor advances past stackLimit as soon as even one procedure
    // compiles — at that point the translator's own live-recursion floor
    // tracks arenaCursor instead of the flat stackLimit. Every other
    // Executor::onStack TEST here crosses that line incidentally (a
    // generous leftover just makes it harmless); this one leaves only what
    // the measured chain actually needs, so the crossing decides whether
    // this succeeds. It is also the case where arenaCeiling() is bound by
    // the live stack floor rather than by the arena's own end: nothing
    // sizes the arena short of the code limit any more, so the two sides
    // checking each other is the only thing keeping them apart.
    const Instr proc0Body[] = {CONST(5), call(1), bare(Op::RETURN)};
    const Instr proc1Body[] = {LOAD(0), call(2), opImm(Op::ADD, 1), bare(Op::RETURN)};
    const Instr proc2Body[] = {LOAD(0), opImm(Op::ADD, 100), bare(Op::RETURN)};
    uint8_t measureBytes0[16], measureBytes1[16], measureBytes2[16];
    Proc measureProcs[] = {
        makeProc(0, proc0Body, 3, measureBytes0, sizeof(measureBytes0)),
        makeProc(1, proc1Body, 4, measureBytes1, sizeof(measureBytes1)),
        makeProc(1, proc2Body, 3, measureBytes2, sizeof(measureBytes2)),
    };
    uint32_t argCounts[] = {0, 1, 1};
    bool savesLR[] = {true, true, false}; // proc0Body/proc1Body each CALL; proc2Body doesn't
    uint32_t tightArena = 4; // + each measured procedure's own size, below
    for(uint32_t i = 0; i < 3; i++)
    {
        tightArena += measuredHalfwords(measureProcs[i], i, argCounts, 3, savesLR[i]) * 2;
    }

    ProcSource procs[] = {{0, proc0Body, 3}, {1, proc1Body, 4}, {1, proc2Body, 3}};
    uint8_t bytes[48];
    uint32_t maxCallDepth = 2, totalDepth = 2, procCount = 3;
    uint32_t len = encodeJitProgram(maxCallDepth, totalDepth, procs, procCount, bytes, sizeof(bytes));

    static constexpr uint32_t TIGHT_TEST_SLACK = BOUNDARY_SLACK + 512;
    uint32_t stackLimit = currentSp()
        - requiredStackBytesFor(procCount, totalDepth, maxCallDepth) - tightArena - TIGHT_TEST_SLACK;

    ProgramResult r = Executor::onStack(stackLimit, /*interruptReserve=*/0).run(bcMapped(bytes), len, nullptr, 0);

    if(r.trapped)
    {
        writeHexTrap(r.value);
    }
    CHECK(!r.trapped);
    CHECK(r.value == 106);
}

static uint32_t g_extHelperStackBytes = 0;
extern "C" uint32_t extHelperStackBytes() { return g_extHelperStackBytes; }

TEST(AnExtensionsDeclaredHelperStackCountsOnlyPastTheTranslatorsOwnEntry)
{
    // Same program, same stackLimit — the only difference is what an extension
    // declares. If that declaration doesn't reach requiredStackBytes the
    // reservation stops being a bound at the one moment it matters: a helper
    // runs at the deepest point of an excursion. And if it is simply added to
    // the translator's entry cost rather than measured against it, programs
    // that fit are refused — the two never share the stack.
    //
    // The program contains no extension opcodes, so decode is never called;
    // helperStackBytes is consulted regardless.
    const Instr body[] = {CONST(37), bare(Op::RETURN)};
    ProcSource procs[] = {{0, body, 2}};
    uint8_t bytes[32];
    uint32_t maxCallDepth = 0, totalDepth = 1, procCount = 1;
    uint32_t len = encodeJitProgram(maxCallDepth, totalDepth, procs, procCount, bytes, sizeof(bytes));

    uint32_t stackLimit = currentSp() - requiredStackBytesFor(procCount, totalDepth, maxCallDepth) - ARENA_ALLOWANCE;

    ProgramResult without = Executor::onStack(stackLimit, /*interruptReserve=*/0).run(bcMapped(bytes), len, nullptr, 0);
    if(without.trapped)
    {
        writeHexTrap(without.value);
    }
    CHECK(!without.trapped);
    CHECK(without.value == 37);

    // Over the translator's entry cost, but only by less than the leftover:
    // charged as a sum this is 924 bytes and would be refused, charged as the
    // deeper of the two it is 412 and fits.
    g_extHelperStackBytes = ARENA_ALLOWANCE + BOUNDARY_SLACK;
    ProgramResult absorbed = Executor::onStack(stackLimit, /*interruptReserve=*/0).run(bcMapped(bytes), len, nullptr, 0);
    g_extHelperStackBytes = 0;
    if(absorbed.trapped)
    {
        writeHexTrap(absorbed.value);
    }
    CHECK(!absorbed.trapped);
    CHECK(absorbed.value == 37);

    // Far enough past it that the excess alone outgrows the leftover.
    g_extHelperStackBytes = ARENA_ALLOWANCE + TRANSLATOR_ENTRY_WORST_CASE_BYTES + BOUNDARY_SLACK;
    ProgramResult over = Executor::onStack(stackLimit, /*interruptReserve=*/0).run(bcMapped(bytes), len, nullptr, 0);
    g_extHelperStackBytes = 0;
    CHECK(over.trapped == LANDING_RESOURCE_ERROR);
    CHECK(over.value == RESOURCE_EXHAUSTED_STACK_BUDGET);
}

TEST(TheInterruptReserveIsAddedToTheUpFrontBudget)
{
    // interruptReserve is the one budget term with no code path of its own —
    // no instruction ever spends it, an exception does, whenever it likes. So
    // nothing but this would notice it going missing from requiredStackBytes.
    const Instr body[] = {CONST(37), bare(Op::RETURN)};
    ProcSource procs[] = {{0, body, 2}};
    uint8_t bytes[32];
    uint32_t maxCallDepth = 0, totalDepth = 1, procCount = 1;
    uint32_t len = encodeJitProgram(maxCallDepth, totalDepth, procs, procCount, bytes, sizeof(bytes));

    uint32_t stackLimit = currentSp() - requiredStackBytesFor(procCount, totalDepth, maxCallDepth) - ARENA_ALLOWANCE;

    ProgramResult without = Executor::onStack(stackLimit, /*interruptReserve=*/0).run(bcMapped(bytes), len, nullptr, 0);
    if(without.trapped)
    {
        writeHexTrap(without.value);
    }
    CHECK(!without.trapped);
    CHECK(without.value == 37);

    // A real exception frame still fits, and compiles through an arena ceiling
    // that is now holding that much back from the live stack floor.
    ProgramResult modest = Executor::onStack(stackLimit, ARMV6M_EXCEPTION_FRAME_BYTES).run(bcMapped(bytes), len, nullptr, 0);
    if(modest.trapped)
    {
        writeHexTrap(modest.value);
    }
    CHECK(!modest.trapped);
    CHECK(modest.value == 37);

    // Past the leftover that made it fit, the code limit drops below stackLimit.
    ProgramResult with = Executor::onStack(stackLimit, ARENA_ALLOWANCE + BOUNDARY_SLACK).run(bcMapped(bytes), len, nullptr, 0);
    CHECK(with.trapped == LANDING_RESOURCE_ERROR);
    CHECK(with.value == RESOURCE_EXHAUSTED_STACK_BUDGET);
}

// Deliberately more generous than this file's own STACK_SLACK_ABOVE_BSS:
// what this TEST measures is translator recursion depth, so the floor must
// be nowhere near the thing under test.
static constexpr uint32_t DEEP_NESTING_SLACK = 512;

static uint32_t deepNestingStackLimit()
{
    return (uint32_t)(uintptr_t)&__bss_end + DEEP_NESTING_SLACK;
}

TEST(DeepNestingStaysWithinStackBudget)
{
    // 8 levels of BR_TABLE(1) (if-then) nesting — the same depth and
    // shape as test/host/test_translate_proc.cpp's own
    // NestedIfChainReportsOverflowWithTheSameSlackADepthZeroBodyTolerates,
    // here run through the real translator at real -Os on real hardware
    // instead of an -O0 host build standing in for it. Each level is a
    // real BR_TABLE(1) -> translateIfThen -> processUntilTerminator ->
    // processNonTerminators chain (translateIfThen itself is confirmed
    // inlined into processNonTerminators at this optimization level — so
    // the per-level cost this measures is
    // processNonTerminators + processUntilTerminator's real combined
    // frame, not a hypothetical one — tools/stack-margin.ts reports the
    // same inlining.)
    constexpr int kDepth = 8;
    Instr body[2 * kDepth + 2];
    for(int i = 0; i < kDepth; i++)
    {
        body[i] = brTable(1);
    }
    for(int i = 0; i < kDepth; i++)
    {
        body[kDepth + i] = bare(Op::BLOCK_END);
    }
    body[2 * kDepth] = CONST(0);
    body[2 * kDepth + 1] = bare(Op::RETURN);

    ProcSource procs[] = {{0, body, (uint32_t)(2 * kDepth + 2)}};
    uint8_t progBytes[256];
    uint32_t progLen = encodeJitProgram(/*maxCallDepth=*/0, /*totalDepth=*/0, procs, 1, progBytes, sizeof(progBytes));

    static uint8_t arena[512];
    ProgramResult r = Executor::split((uint32_t)(uintptr_t)arena, sizeof(arena), deepNestingStackLimit(), /*interruptReserve=*/0)
        .run(bcMapped(progBytes), progLen, nullptr, 0);

    // Either outcome is healthy and worth distinguishing in the report:
    // a clean RESOURCE_ERROR proves the live checks fired before anything
    // dangerous happened; a real result proves this depth compiled and
    // ran with the margin checked below intact. A hang or a wild jump
    // (into .bss, into unmapped flash) is the only actual failure mode,
    // and would show up as this whole TEST never reporting at all rather
    // than as a clean CHECK() failure — the high-water-mark scan after
    // runAllTests below is what actually confirms nothing got that close.
    if(r.trapped)
    {
        writeHexTrap(r.value);
    }
    else
    {
        writeHexResult(r.value);
    }
}
