/* jit-armv6m/runtime — the entry procedure's incoming argument layout.
 *
 * isa-core.md §2.3/§5.5 put no ceiling on procedure 0's own arg_count, and
 * validateProgram approves any of them — but the entry procedure has no
 * caller to place its arguments the way a CALL site does, so enterDispatch
 * has to be that caller. This header is the half of that job worth doing
 * in C: which value ends up in which window register, and which ones stay
 * on the stack.
 *
 * The layout is not a choice — it is whatever an ordinary CALL already
 * produces, because procedure 0's compiled prologue and epilogue are the
 * same code any callee gets. window.cpp's spillForCall + fillCalleeArgs
 * (a call site's own shuffle, for stackArgs = arg_count - 1) leave exactly:
 *
 *   slots 0 .. N-5   on the stack, slot 0 FURTHEST from sp
 *                    (Window::spillOffset's own rawSpillOffset(N,k) =
 *                    4*(N-5-k)), reclaimed by the callee on the way out
 *   slots N-4 .. N-2 in window registers physReg(k)
 *   slot  N-1        in acc — translate_proc.cpp's entry flush writes it
 *                    into physReg(N-1) itself, which is why nothing here
 *                    supplies that register
 *
 * Doing the physReg(k) % WINDOW_SIZE rotation here rather than in
 * runtime.S is what keeps the asm to a push loop and five loads: the
 * rotation depends on N mod 4, which is a four-way branch in assembly and
 * one array index in C. It also means the rotation is exercised by an
 * ordinary host test (test/host/test_entry_args.cpp) instead of only by a
 * hung emulator, and that it calls the translator's *real* physReg rather
 * than a second copy of the formula that could drift from it.
 */
#ifndef JIT_ARMV6M_RUNTIME_ENTRY_ARGS_H_
#define JIT_ARMV6M_RUNTIME_ENTRY_ARGS_H_

#include <stdint.h>
#include <stddef.h>

#include "runtime_host.h" /* ENTRY_ARGS_*_OFFSET */
#include "window.h"       /* jitc::physReg, and WINDOW_BASE/WINDOW_SIZE via registers.h */

/* What enterDispatch reads to marshal the entry procedure's arguments.
 * Built once per excursion by enterProgramCore, in its own frame, and
 * dead the moment enterDispatch branches to callHelper. */
struct EntryArgs
{
    /* args[0 .. spilledCount-1], aliased directly — never copied. They are
     * already contiguous and already in ascending slot order in the
     * caller's own vector, and enterDispatch pushes them in that order, so
     * a bounce buffer would buy nothing and would put an argument-sized
     * array back in this frame. */
    const uint32_t *spilled;

    /* max(0, arg_count - WINDOW_SIZE) — how many of the entry procedure's
     * arguments live below its window and so have to be pushed. */
    uint32_t spilledCount;

    /* window[i] is destined for register WINDOW_BASE + i. The entry for
     * physReg(arg_count-1) is deliberately left zero: the prologue writes
     * that register from acc. enterDispatch loads all WINDOW_SIZE of them
     * unconditionally, so this must be zero-initialized rather than merely
     * assigned where it matters. */
    uint32_t window[jitc::WINDOW_SIZE];

    /* args[arg_count-1] — isa-core.md §4.6's acc-borne last argument. Zero
     * when arg_count is 0, where the entry procedure reads no arguments at
     * all and acc starts dead (§8.7). */
    uint32_t acc;
};

/* The four offsets runtime.S hardcodes, checked against the real layout.
 * Conditioned on a 32-bit pointer: `spilled` is the only member whose
 * width varies, and the asm these numbers belong to only ever runs on the
 * target. A 64-bit host build lays the struct out wider and is not wrong
 * to. */
#if UINTPTR_MAX == 0xFFFFFFFFu
static_assert(offsetof(EntryArgs, spilled) == ENTRY_ARGS_SPILLED_OFFSET,
    "runtime.S's entry-argument block reads ea->spilled at ENTRY_ARGS_SPILLED_OFFSET");
static_assert(offsetof(EntryArgs, spilledCount) == ENTRY_ARGS_SPILLED_COUNT_OFFSET,
    "runtime.S's entry-argument block reads ea->spilledCount at ENTRY_ARGS_SPILLED_COUNT_OFFSET");
static_assert(offsetof(EntryArgs, window) == ENTRY_ARGS_WINDOW_OFFSET,
    "runtime.S's entry-argument block reads ea->window at ENTRY_ARGS_WINDOW_OFFSET");
static_assert(offsetof(EntryArgs, acc) == ENTRY_ARGS_ACC_OFFSET,
    "runtime.S's entry-argument block reads ea->acc at ENTRY_ARGS_ACC_OFFSET");
#endif

/* runtime.S loads the window as four unrolled LDRs into r4-r7 by name, so
 * both of these are baked into the asm, not just into the arithmetic
 * below. */
static_assert(jitc::WINDOW_SIZE == 4 && jitc::WINDOW_BASE == 4,
    "runtime.S's entry-argument block unrolls the window fill as ldr r4-r7");

/* Split `args[0..declared-1]` into the three places the calling convention
 * puts them. `declared` is the entry procedure's own arg_count, as recorded
 * by Runtime::init — the caller-supplied count is checked against it before
 * this runs, so the two agree by construction here. */
inline void buildEntryArgs(EntryArgs *ea, const uint32_t *args, uint32_t declared)
{
    *ea = EntryArgs{}; /* zeroes window[], including physReg(declared-1)'s own slot */

    if(declared == 0)
    {
        return; /* nothing to place, and `args` may legitimately be null */
    }

    ea->acc = args[declared - 1];

    /* One quantity, two readings: the lowest slot the window covers, and
     * therefore also the count of slots below it. */
    const uint32_t windowFloor = declared > jitc::WINDOW_SIZE ? declared - jitc::WINDOW_SIZE : 0;

    /* k + 1 < declared, not k < declared: slot declared-1 arrives via acc. */
    for(uint32_t k = windowFloor; k + 1 < declared; k++)
    {
        ea->window[jitc::physReg(k) - jitc::WINDOW_BASE] = args[k];
    }

    ea->spilled = args;
    ea->spilledCount = windowFloor;
}

#endif /* JIT_ARMV6M_RUNTIME_ENTRY_ARGS_H_ */
