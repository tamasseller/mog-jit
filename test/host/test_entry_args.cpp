// Does enterDispatch's argument marshalling put the entry procedure's
// arguments exactly where an ordinary CALL site would?
//
// It has to: procedure 0 gets the same compiled prologue and epilogue as any
// callee, so the only thing that can make it read its arguments correctly is
// arriving at the layout window.cpp's spillForCall + fillCalleeArgs produce.
// Nothing else in the tree checks that — a CALL 0 can only appear in a
// procedure unreachable from procedure 0 (any reachable path would be a
// call-graph cycle), and unreachable procedures are never dispatched, so
// never translated. enterDispatch is the only live producer of this layout,
// and before entry_args.h there was no producer at all.
//
// So this does not restate buildEntryArgs's own formula back at it. It runs
// the real emitter, interprets the PUSH/POP stream it produces against a
// simulated register file and stack, and requires the descriptor to land on
// a byte-identical machine image.
#include "Test.h"
#include "assembler.h"
#include "window.h"
#include "armv6.h"

#include "runtime_internal.h"
#include "entry_args.h"
#include "host_runtime_support.h"

using namespace jitc;

namespace
{

// A register file plus a word-addressed stack, enough to execute the only
// two instructions either path emits.
struct Machine
{
    static constexpr int TOP = 48;

    uint32_t regs[8] = {};
    uint32_t mem[64] = {};
    int sp = TOP;

    /* PUSH{list}/POP{list} both address the list lowest-register-number to
     * lowest-address; PUSH pre-decrements sp by the whole list, POP
     * post-increments. That single rule is the entire reason physReg
     * descends in k (window.h's header), so modelling it exactly is the
     * point rather than an implementation detail. */
    void push(uint32_t mask)
    {
        sp -= popcount(mask);
        int j = 0;
        for(int r = 0; r < 8; r++) if(mask & (1u << r)) mem[sp + j++] = regs[r];
    }

    void pop(uint32_t mask)
    {
        int j = 0;
        for(int r = 0; r < 8; r++) if(mask & (1u << r)) regs[r] = mem[sp + j++];
        sp += popcount(mask);
    }

    /* What the callee's own code would read for frame slot k at tos == n:
     * a window register, or an SP-relative spill slot at the offset
     * Window::spillOffset names. */
    uint32_t slot(uint32_t n, uint32_t k) const
    {
        if(inWindow(n, k)) return regs[physReg(k)];
        Window w(n, /*savesLR=*/false);
        return mem[sp + (int)(w.spillOffset(k) / 4)];
    }

    /* Compares the state a compiled procedure can actually observe at
     * entry: sp, the operand stack, acc (r0) and the window (r4-r7).
     * r1-r3 are deliberately excluded — they are scratch on both sides
     * (callHelper and the prologue stub clobber all three before the body
     * runs), and this simulation's own push temp lands in r1, which no
     * call site has any reason to touch. */
    bool sameAs(const Machine &o) const
    {
        if(sp != o.sp) return false;
        if(regs[ACC_REG] != o.regs[ACC_REG]) return false;
        for(uint32_t i = 0; i < WINDOW_SIZE; i++)
        {
            if(regs[WINDOW_BASE + i] != o.regs[WINDOW_BASE + i]) return false;
        }
        for(int i = sp; i < TOP; i++) if(mem[i] != o.mem[i]) return false;
        return true;
    }

    static int popcount(uint32_t m) { int c = 0; while(m) { c += m & 1u; m >>= 1; } return c; }
};

/* Interpret one emitted halfword. Both paths emit nothing but T1 PUSH
 * (0xB4xx) and POP (0xBCxx), so anything else is a real change in what the
 * shuffle is made of and should fail loudly rather than be skipped. */
bool step(Machine &m, uint16_t hw)
{
    if((hw & 0xFE00u) == 0xB400u) { m.push(hw & 0xFFu); return true; }
    if((hw & 0xFE00u) == 0xBC00u) { m.pop(hw & 0xFFu); return true; }
    return false;
}

/* Path A — a real CALL site. The caller's own tos is exactly the stack-arg
 * count, so caller slot k holds callee slot k and no unrelated locals are in
 * the way. Seeds that caller state, emits the real shuffle, runs it, then
 * applies the callee prologue's own acc flush. */
bool viaCallSite(Machine &m, const uint32_t *args, uint32_t n)
{
    const uint32_t stackArgs = n - 1; // isa-core.md §4.6: the last argument travels in acc
    Window caller(stackArgs);

    /* Drop sp for whatever the caller has already spilled *before* writing
     * anything: spillOffset is relative to the caller's own current sp, so
     * the two have to be established in that order. */
    m.sp -= (int)(stackArgs > WINDOW_SIZE ? stackArgs - WINDOW_SIZE : 0);

    for(uint32_t k = 0; k < stackArgs; k++)
    {
        if(inWindow(stackArgs, k)) m.regs[physReg(k)] = args[k];
        else                       m.mem[m.sp + (int)(caller.spillOffset(k) / 4)] = args[k];
    }

    uint16_t buf[32];
    Assembler e(buf, 32);
    caller.spillForCall(e, stackArgs);
    Window::fillCalleeArgs(e, stackArgs);

    for(uint32_t i = 0; i < e.halfwordCount(); i++)
    {
        if(!step(m, buf[i])) return false;
    }

    /* A call site materializes the last argument into acc before branching,
     * so r0 holds it at callee entry on this path too — model it, or the
     * image comparison below would flag a difference that isn't one. */
    m.regs[0] = args[n - 1];
    m.regs[physReg(n - 1)] = args[n - 1]; // translate_proc.cpp's entry flush
    return true;
}

/* Path B — enterDispatch, as runtime.S executes it: push ea->spilled in
 * ascending order, load window[i] into WINDOW_BASE + i, then acc. */
void viaEnterDispatch(Machine &m, const uint32_t *args, uint32_t n)
{
    EntryArgs ea;
    buildEntryArgs(&ea, args, n);

    for(uint32_t i = 0; i < ea.spilledCount; i++)
    {
        m.regs[1] = ea.spilled[i];
        m.push(1u << 1); // push {r1}
    }
    for(uint32_t i = 0; i < WINDOW_SIZE; i++) m.regs[WINDOW_BASE + i] = ea.window[i];

    m.regs[0] = ea.acc;
    if(n >= 1) m.regs[physReg(n - 1)] = ea.acc; // the prologue's own flush, as above
}

} // namespace

TEST(entryArgsMatchesACallSiteForEveryArgCount)
{
    // 12 covers both window phases and four distinct n % WINDOW_SIZE
    // rotations, well past the k % 4 wrap that makes this worth testing.
    for(uint32_t n = 1; n <= 12; n++)
    {
        uint32_t args[16];
        for(uint32_t k = 0; k < n; k++) args[k] = 0x1000u + k; // distinct, so a swap shows

        Machine viaCall;
        CHECK(viaCallSite(viaCall, args, n)); // false == the shuffle emitted something other than PUSH/POP

        Machine viaEntry;
        viaEnterDispatch(viaEntry, args, n);

        // The invariant that actually matters: the callee reads args[k] at
        // slot k. Checked on both paths, so a shared misunderstanding of the
        // layout cannot hide.
        for(uint32_t k = 0; k < n; k++)
        {
            CHECK(viaCall.slot(n, k) == args[k]);
            CHECK(viaEntry.slot(n, k) == args[k]);
        }

        // And the stronger claim: the two arrive at the same observable
        // machine state, not merely at one both happen to read correctly.
        // This is what a wrong window[] index or a reversed spill loop
        // breaks, and it holds for every n from 1 to 12 — both window
        // phases, all four k % WINDOW_SIZE rotations, and 0 through 8
        // spilled words.
        CHECK(viaEntry.sameAs(viaCall));
    }
}

TEST(entryArgsLeavesTheAccRegisterToThePrologue)
{
    // physReg(n-1) is deliberately not supplied by the descriptor — the
    // prologue writes it from acc. If buildEntryArgs ever filled it in,
    // window[] would disagree with acc for no reason, so pin it.
    for(uint32_t n = 1; n <= 8; n++)
    {
        uint32_t args[8];
        for(uint32_t k = 0; k < n; k++) args[k] = 0x2000u + k;

        EntryArgs ea;
        buildEntryArgs(&ea, args, n);

        CHECK(ea.window[physReg(n - 1) - WINDOW_BASE] == 0);
        CHECK(ea.acc == args[n - 1]);
        CHECK(ea.spilledCount == (n > WINDOW_SIZE ? n - WINDOW_SIZE : 0));
        CHECK(ea.spilled == (ea.spilledCount > 0 ? args : nullptr) || ea.spilledCount == 0);
    }
}

TEST(entryArgsForZeroArgumentsPlacesNothing)
{
    EntryArgs ea;
    buildEntryArgs(&ea, nullptr, 0); // args is legitimately null here

    CHECK(ea.spilledCount == 0);
    CHECK(ea.acc == 0);
    for(uint32_t i = 0; i < WINDOW_SIZE; i++) CHECK(ea.window[i] == 0);
}

TEST(entryArgsSpillsAscendingSoSlotZeroIsFurthestFromSp)
{
    // The one ordering a single spilled word cannot catch: n=5 spills
    // exactly one, so a reversed loop is invisible there. n=6 upward is
    // where the direction is observable at all.
    uint32_t args[8];
    for(uint32_t k = 0; k < 8; k++) args[k] = 0x3000u + k;

    EntryArgs ea;
    buildEntryArgs(&ea, args, 6);
    CHECK(ea.spilledCount == 2);
    CHECK(ea.spilled[0] == args[0]); // pushed first, so furthest from sp
    CHECK(ea.spilled[1] == args[1]);

    // Confirmed against the offsets the callee will actually use.
    Window w(6, /*savesLR=*/false);
    CHECK(w.spillOffset(0) == 4);
    CHECK(w.spillOffset(1) == 0);
}
