// jit-armv6m/compiler — the register window, ported verbatim (formulas and
// all) from jit-armv6m/prototype/src/window.ts. See that file's own header
// comment for the full rationale (physReg's deliberately descending cyclic
// direction, sp genuinely tracking spilled depth with no fixed per-
// procedure reservation, the three PUSH/POP batching cases) — this port
// preserves every formula exactly, on the assumption that re-deriving them
// from scratch would risk reintroducing bugs window.ts's own comments
// document as already found and fixed (e.g. fillCalleeArgs's WINDOW_SIZE-1
// cap).
//
// restoreWindow (block-exit truncation) is NOT ported — no blocks this
// slice (BR_TABLE/LOOP are out of scope).
#ifndef JIT_ARMV6M_COMPILER_WINDOW_H_
#define JIT_ARMV6M_COMPILER_WINDOW_H_

#include <cstdint>
#include "registers.h"

namespace jitc {

class Emitter;
class AccState;

bool inWindow(uint32_t tos, uint32_t k);

/** Physical register holding frame-relative slot k, valid only when
 *  inWindow(tos, k). Deliberately descending in k — see this file's
 *  header. */
uint32_t physReg(uint32_t k);

/** Window state for one procedure's translation — the tos counter, public
 *  exactly like window.ts's own Window class (whose callers read/write
 *  .tos directly at block/call boundaries), plus the savesLR fact needed
 *  by spillOffset()/discardWindow() (below) — ported from window.ts's own
 *  promotion of these from free functions to methods, once a second piece
 *  of per-procedure state needed to feed the same formulas. */
class Window {
public:
    uint32_t tos;

    explicit Window(uint32_t argCount, bool savesLR = false)
        : tos(argCount), savesLR_(savesLR),
          initialSpilledCount_(argCount > WINDOW_SIZE ? argCount - WINDOW_SIZE : 0) {}

    /** The current top slot's physical register. */
    uint32_t topReg() const { return physReg(tos - 1); }

    /** Push accState's current value onto the window: spill whatever it's
     *  about to evict, materialize the value into its new home, bump tos. */
    void pushValue(Emitter &e, AccState &accState);

    /** Complete a pop whose value was already read out of topReg() — the
     *  fill (if any) and the tos decrement, together. Must be called only
     *  once every read of topReg() has already been emitted. */
    void finishPop(Emitter &e);

    /** Byte offset from the current sp for slot k, valid only when k is
     *  genuinely spilled (!inWindow(tos, k)). savesLR procedures (this
     *  procedure makes its own nested CALL) push {lr} in their own
     *  prologue, before this procedure's body ever reads anything — which
     *  lands strictly between wherever the caller left sp and this
     *  procedure's own first read of one of its own out-of-window
     *  arguments (k < initialSpilledCount), shifting those specific slots
     *  one word further from sp than the caller's own bookkeeping
     *  assumed. Locals this procedure spills itself later (k outside that
     *  range) need no adjustment — spilled strictly after that same
     *  push{lr}, so this procedure's own view of sp is already
     *  self-consistent for those. */
    uint32_t spillOffset(uint32_t k) const;

    /** A terminator's (RETURN/TRAP) sp rebalancing — nothing downstream
     *  ever reads r4-r7 again, so there's nothing to reload for, just a
     *  bare sp adjustment undoing every spill this procedure's own body
     *  made. savesLR procedures only reclaim their own self-spilled
     *  locals here, not the initialSpilledCount slots a caller placed
     *  before this procedure's own prologue ran (those sit below this
     *  procedure's own push{lr} — reclaiming them here would walk past
     *  the saved lr word without reading it). abi_strategy.cpp's
     *  abiEmitReturn is what reclaims that remainder, after retrieving
     *  the saved record. */
    void discardWindow(Emitter &e) const;

private:
    bool savesLR_;
    uint32_t initialSpilledCount_;
};

/** CALL's own shuffle, first half — spills the caller's currently-resident
 *  window into the leftover-locals mask (one plain PUSH) and the
 *  stack-passed args (pushLargestKClosest). Doesn't move window.tos. */
void spillForCall(Emitter &e, Window &window, uint32_t stackArgs);

/** CALL's shuffle, second half — fills the callee's own canonical phase-0
 *  window from what spillForCall just pushed. Capped at WINDOW_SIZE - 1,
 *  not WINDOW_SIZE: the callee's own last argument (delivered via acc, not
 *  through this function) always lands at physReg(argCount-1), which is
 *  the same physical register as physReg(0) whenever stackArgs equals
 *  WINDOW_SIZE exactly — this is a real, previously-found off-by-one and
 *  must be preserved exactly. */
void fillCalleeArgs(Emitter &e, uint32_t stackArgs);

/** CALL's shuffle, final step — once the callee returns, reload whatever
 *  spillForCall/fillCalleeArgs consumed. Mutates window.tos to targetTos. */
void reloadAfterCall(Emitter &e, Window &window, uint32_t targetTos);

} // namespace jitc

#endif // JIT_ARMV6M_COMPILER_WINDOW_H_
