// Empirical corroboration for docs/design.md's stack-safety strategy
// (G2/G3/G5): fills the stack region with a sentinel before any test
// runs, then after the whole suite completes, scans for how far the
// sentinel actually got overwritten. This is the only independent check
// available on this hardware — Cortex-M0 has no MPU, no MSPLIM, and
// vectors.S never switches to PSP, so main()/1test/the JIT's own
// recursive scanBody/translateBody C-stack all share one flat MSP stack
// with nothing hardware-enforced to catch an overflow. linker.ld's own
// header already documents a real incident of exactly this (a run that
// walked sp down far enough to collide with .bss, confirmed via
// `qemu-system-arm -d exec`) — this file exists so that class of bug
// shows up as a reported number on every run instead of being
// rediscovered the same way again.
#include <cstdint>
#include "stack_paint.h"
#include "instr.h"
#include "encode_instr.h"
#include "translate_proc.h"
#include "runtime.h"
#include "executor.h"
#include "Test.h"
#include "semihosting_output.h"

using namespace jitc;

extern "C" uint8_t __bss_end; // vectors.S/linker.ld — see main.cpp's own extern for the same symbol
extern "C" uint8_t _stack_top; // linker.ld: _stack_top = ORIGIN(ram) + LENGTH(ram); vectors.S's own initial sp

static constexpr uint8_t PAINT_BYTE = 0xAA;

// How much sentinel must survive directly above __bss_end: the measured
// distance between .bss and the deepest address anything in this image
// touched. Deliberately not derived from stack_budget.h — the point is
// empirically observed headroom, not agreement with the constants this file
// exists to double-check.
//
// The value is main.cpp's own GENEROUS_SLACK, the lowest stackLimit any TEST
// in this image declares and therefore the floor every one of them promises
// to stay above. An excursion's arena base sits exactly there, so a healthy
// run lands on this number rather than over it; anything less means something
// crossed a floor it had been checked against.
static constexpr uint32_t REQUIRED_HEADROOM_BYTES = 128;

// Real incident, found via this exact file: at -Os, GCC rewrites a plain
// byte-fill loop into a call to memset(). That call needs its own stack
// frame (at minimum, somewhere to push lr) — and since paintStack() is
// called as main()'s very first statement, the live sp at that instant is
// only a few bytes below _stack_top (confirmed via `qemu-system-arm -d
// exec`, the same diagnostic linker.ld's own header cites for the
// original incident this file exists to catch: 0x20001ff0 vs a
// _stack_top of 0x20002000). Painting all the way up to that pre-call sp
// meant memset's *own* just-pushed return address fell inside the range
// it was told to fill — memset overwrote its own return address with the
// sentinel byte and jumped into 0xAAAAAAAA on return, landing in the
// fault vector's hang spin-loop. Two independent fixes, kept both
// deliberately rather than picking one: `volatile` blocks the loop from
// being rewritten into any such call in the first place, and this margin
// leaves headroom below the measured sp regardless, in case a future
// compiler/flag change reintroduces a similar rewrite some other way.
static constexpr uint32_t PAINT_CALL_SAFETY_MARGIN = 64;

// Called as literally the first line of main(), before TestRunner pushes any
// frames of its own — everything below the current sp (minus
// PAINT_CALL_SAFETY_MARGIN, see above) at that instant is
// provably unused, the same reasoning the startup .bss-zero loop already
// relies on for its own bound.
void paintStack()
{
    uint8_t *lo = &__bss_end;
    register uint32_t sp asm("sp");
    uint8_t *hi = (uint8_t *)(uintptr_t)sp - PAINT_CALL_SAFETY_MARGIN;
    for(volatile uint8_t *p = lo; p < hi; p++)
    {
        *p = PAINT_BYTE;
    }
}

// Scans forward from __bss_end for the first byte that's no longer the
// sentinel — the lowest address anything actually touched, i.e. the
// closest the whole test run's real stack usage came to colliding with
// .bss. Called once, after every other TEST has run, so this reflects the
// deepest point *anything* in this binary reached — not an isolated
// microbenchmark of one scenario in artificial isolation. `volatile`
// here too — read-only so it can never repeat paintStack()'s own
// self-overwrite hazard, but a rewrite into a libc memchr-alike would
// still risk a *false* reading if that call's own transient stack frame
// landed inside the scanned range close to _stack_top.
static uint32_t highWaterMarkBytesFromBssEnd()
{
    uint8_t *lo = &__bss_end;
    uint8_t *hi = &_stack_top;
    for(volatile uint8_t *p = lo; p < hi; p++)
    {
        if(*p != PAINT_BYTE)
        {
            return (uint32_t)(p - lo);
        }
    }
    return (uint32_t)(hi - lo); // nothing touched at all — the whole region is still painted
}

static uint32_t currentSp()
{
    register uint32_t sp asm("sp");
    return sp;
}

// Same "anchor above __bss_end, not below the measured sp" reasoning as
// main.cpp's own stackLimitAboveBss() — duplicated locally rather than
// shared via a header, matching this file set's existing convention of
// each TEST file owning its own small helpers (main.cpp does the same).
static uint32_t stackLimitAboveBss()
{
    static constexpr uint32_t GENEROUS_SLACK = 512;
    return (uint32_t)(uintptr_t)&__bss_end + GENEROUS_SLACK;
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
    ProgramResult r = Executor::split((uint32_t)(uintptr_t)arena, sizeof(arena), stackLimitAboveBss(), /*interruptReserve=*/0)
        .run(progBytes, progLen, nullptr, 0);

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

// Not a TEST — deliberately runs standalone from main(), after
// runAllTests, so its result reflects the whole suite's own worst case
// rather than being folded into 1test's own pass/fail accounting for one
// isolated case. Prints the byte-count unconditionally (so a human
// reading QEMU's own semihosting output always sees the real number, pass
// or fail) and returns whether the required slack held.
bool reportStackHighWaterMark()
{
    uint32_t headroom = highWaterMarkBytesFromBssEnd();
    uint32_t total = (uint32_t)(&_stack_top - &__bss_end);

    semihostingWrite0("STACK_HEADROOM_BYTES_ABOVE_BSS_END:");
    writeHexResult(headroom);
    semihostingWrite0("STACK_BYTES_CONSUMED:");
    writeHexResult(total - headroom);

    return headroom >= REQUIRED_HEADROOM_BYTES;
}
