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

class Emitter;
class AccState;

bool inWindow(uint32_t tos, uint32_t k);

// Physical register holding frame-relative slot k, valid only when
// inWindow(tos, k). Deliberately descending in k — see this file's header.
uint32_t physReg(uint32_t k);

// Window state for one procedure's translation: the tos counter, plus the
// savesLR fact needed by spillOffset()/discardWindow() below.
class Window
{
public:
    uint32_t tos;

    explicit Window(uint32_t argCount, bool savesLR = false)
        : tos(argCount), savesLR(savesLR),
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
    void pushValue(Emitter &e, AccState &accState);

    // Complete a pop whose value was already read out of topReg() — the
    // fill (if any) and the tos decrement, together. Must be called only
    // once every read of topReg() has already been emitted.
    void finishPop(Emitter &e);

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

    // A terminator's (RETURN/TRAP) sp rebalancing — nothing downstream ever
    // reads r4-r7 again, so this is just a bare sp adjustment undoing every
    // spill this procedure's own body made. A savesLR procedure only
    // reclaims its own self-spilled locals here, not the
    // initialSpilledCount slots its caller placed before this procedure's
    // own prologue ran (those sit below this procedure's own push{lr}).
    // abi_strategy.cpp's abiEmitReturn reclaims that remainder, after
    // retrieving the saved record.
    void discardWindow(Emitter &e) const;

private:
    bool savesLR;
    uint32_t initialSpilledCount;
};

// CALL's own shuffle, first half — spills the caller's currently-resident
// window into the leftover-locals mask (one plain PUSH) and the
// stack-passed args (pushLargestKClosest). Doesn't move window.tos.
void spillForCall(Emitter &e, Window &window, uint32_t stackArgs);

// CALL's shuffle, second half — fills the callee's own canonical phase-0
// window from what spillForCall just pushed. Capped at WINDOW_SIZE - 1, not
// WINDOW_SIZE: the callee's own last argument (delivered via acc, not
// through this function) always lands at physReg(argCount-1), which is the
// same physical register as physReg(0) whenever stackArgs equals
// WINDOW_SIZE exactly, so this cap must stay exact.
void fillCalleeArgs(Emitter &e, uint32_t stackArgs);

// CALL's shuffle, final step — once the callee returns, reload whatever
// spillForCall/fillCalleeArgs consumed. Mutates window.tos to targetTos.
void reloadAfterCall(Emitter &e, Window &window, uint32_t targetTos);

// blocks.h's own block-exit truncation: any TOS surplus above targetTos is
// implicitly dropped at a BLOCK_END/loop back-edge — no bytecode-level pop
// sequence runs, so sp needs rebalancing here regardless of whether any
// physical register still holds something the target window's own mapping
// expects. What's spilled above targetTos's own ceiling is abandoned
// outright; what's spilled at or below it is genuinely historical data,
// reloaded via popRuns in the same larger-k-first order it was spilled in.
// Mutates window.tos to targetTos directly.
void restoreWindow(Emitter &e, Window &window, uint32_t targetTos);

} // namespace jitc

#endif // JIT_ARMV6M_COMPILER_WINDOW_H_
