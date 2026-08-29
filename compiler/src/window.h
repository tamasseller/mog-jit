// The register window (docs/design.md §5). Pure tos-and-k math, plus the
// spill/fill emission it drives. Doesn't know about the acc fusion state
// machine (accstate.h) or block structure (blocks.h) — just "where does
// frame-relative slot k live right now, and what native code does moving
// tos emit."
//
// tos is the *count* of live slots (starts at argCount, regs[tos++] = v on
// push) — one past the top slot's own index.
//
// sp genuinely tracks the current spilled depth — there is no fixed
// per-procedure reservation. The whole-program stack bound
// (isa-core.md §8.3) is a maximum over call sites and local peaks, not a
// sum, and is only achievable if the underlying storage is actually reused
// as frames come and go. So every ordinary spill here is a real,
// sp-decrementing single-register PUSH, and every fill a real
// sp-incrementing single-register POP, always in ascending-k order — the
// most recently spilled slot is always the one closest to sp. One
// procedure's own spilled locals sit strictly below whatever its caller
// had already spilled, which is what makes frames nest correctly with zero
// cross-procedure bookkeeping.
//
// physReg's cyclic direction is reversed (descending in k), not arbitrary:
// a batched PUSH{list}/POP{list} is equivalent to issuing that list as
// separate single-register instructions in one specific order (descending
// register number for PUSH, ascending for POP). Spills happen in ascending-k
// order; mapping ascending k to descending register number makes that real,
// already-emitted spill sequence exactly equivalent to one hypothetical
// batched PUSH, so a batched POP can read it back later, register-for-
// register, with no reordering trick needed.
//
// Three PUSH/POP consumers share one windowRuns building block:
// 1. Mirrored (spillForCall/reloadAfterCall's leftover-locals mask): a
//    single PUSH of an untouched register set followed later by a POP of
//    that exact same set restores exactly what was pushed, by hardware's
//    own inverse guarantee — no per-k reasoning needed.
// 2. Fresh, remapped (pushLargestKClosest/fillCalleeArgs): CALL's args need
//    to land in the callee's canonical physReg(0).., not wherever they
//    lived in the caller's window. pushLargestKClosest splits the range
//    with windowRuns and pushes the runs forward (pre-wrap, then
//    post-wrap) so the largest k ends up closest to sp, exactly where
//    fillCalleeArgs's ascending batched POP expects it.
// 3. Historical, read back (popRuns): restoreWindow and reloadAfterCall's
//    deeper tail reload data whose physical layout is already fixed by
//    real chronological spills — which, per the identity above, already
//    matches a hypothetical batched PUSH — so popRuns splits the range the
//    same way and pops the runs in reverse (larger-k, closer-to-sp run
//    first).
//
// restoreWindow also has a no-op case: nothing intervenes between a
// truncation point and whatever reads the window next, so any currently-
// resident, still-live-in-the-target register needs no push or pop at all
// (physReg(k) doesn't depend on tos) — only ks below the currently-resident
// window are genuinely historical and go through popRuns.
#ifndef JIT_ARMV6M_COMPILER_WINDOW_H_
#define JIT_ARMV6M_COMPILER_WINDOW_H_

#include <cstdint>
#include "registers.h"

namespace jitc
{

class Assembler;
class AccState;

bool inWindow(uint32_t tos, uint32_t k);

// Physical register holding frame-relative slot k, valid only when
// inWindow(tos, k). Deliberately descending in k — see this file's header.
uint32_t physReg(uint32_t k);

// Window state for one procedure's translation: the tos counter, plus the
// savesLR fact needed by spillOffset()/discardWindow() below.
class Window
{
    bool savesLR;
    uint32_t initialSpilledCount;

public:
    uint32_t tos;

    inline Window() = default;

    explicit Window(uint32_t argCount, bool savesLR = false)
        : tos(argCount), 
          savesLR(savesLR),
          initialSpilledCount(argCount > WINDOW_SIZE ? argCount - WINDOW_SIZE : 0)
    {
    }

    // The current top slot's physical register.
    uint32_t topReg() const
    {
        return physReg(tos - 1);
    }

    // Push accState's current value onto the window: spill whatever it's
    // about to evict, materialize the value into its new home, bump tos.
    void pushValue(Assembler &e, AccState &accState);

    // Complete a pop whose value was already read out of topReg() — the
    // fill (if any) and the tos decrement, together. Must be called only
    // once every read of topReg() has already been emitted.
    void finishPop(Assembler &e);

    // Byte offset from the current sp for slot k, valid only when k is
    // genuinely spilled (!inWindow(tos, k)). A savesLR procedure (one that
    // makes its own nested CALL) pushes {lr} in its own prologue before its
    // body ever reads anything, which lands strictly between wherever the
    // caller left sp and this procedure's own first read of one of its own
    // out-of-window arguments (k < initialSpilledCount) — shifting those
    // specific slots one word further from sp than the caller's own
    // bookkeeping assumed. Locals this procedure spills itself later need
    // no such adjustment.
    uint32_t spillOffset(uint32_t k) const;

    bool discard(Assembler &e) const;

    void spillForCall(Assembler &e, uint32_t stackArgs);

    static void fillCalleeArgs(Assembler &e, uint32_t stackArgs);

    void reloadAfterCall(Assembler &e, uint32_t targetTos);

    bool restore(Assembler &e, uint32_t targetTos);
};


} // namespace jitc

#endif // JIT_ARMV6M_COMPILER_WINDOW_H_
