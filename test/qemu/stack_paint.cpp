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
#include "semihosting_output.h"

extern "C" uint8_t __bss_end; // vectors.S/linker.ld — the TEST files declare the same symbol
extern "C" uint8_t _stack_top; // linker.ld: _stack_top = ORIGIN(ram) + LENGTH(ram); vectors.S's own initial sp

static constexpr uint8_t PAINT_BYTE = 0xAA;

// How much sentinel must survive directly above __bss_end: the measured
// distance between .bss and the deepest address anything in this image
// touched. Deliberately not derived from stack_budget.h — the point is
// empirically observed headroom, not agreement with the constants this file
// exists to double-check.
//
// The value is the TEST files' own STACK_SLACK_ABOVE_BSS, the lowest stackLimit
// any TEST in this image declares and therefore the floor every one of them promises
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
