// jit-armv6m/compiler — block structure (docs/design.md §7.1/§7.2), ported
// from jit-armv6m/prototype/src/blocks.ts. See that file's own header for
// the full rationale this port carries over unchanged: nesting is native
// recursion in translate_proc.cpp (one call per open LOOP/BR_TABLE, this
// file's own Frame held as a plain local), not an explicit stack — the
// property that matters for a no-heap port is "bounded by nesting depth,"
// and a real call stack is already bounded and already there.
//
// Frame is a flat, aggregate-initializable struct with a Kind tag rather
// than a tagged union (instr.h's own header explains the same choice for
// Instr) — the case/loopCond/loopBody shapes share this file's own
// handful of fields, and a plain struct needs no manual lifetime
// management to live as a stack local across translate_proc.cpp's own
// recursive calls. -1 is the "none" sentinel for an int32_t fixup site
// (mirroring translateProc.ts's own `number | null`), since 0 is a valid
// real site offset (the procedure's own first byte).
#ifndef JIT_ARMV6M_COMPILER_BLOCKS_H_
#define JIT_ARMV6M_COMPILER_BLOCKS_H_

#include <cstdint>
#include "instr.h"
#include "shape.h"
#include "armv6.h"

namespace jitc {

class Emitter;
class Window;
class AccState;

enum class FrameKind : uint8_t { Case, LoopCond, LoopBody };

/** Set only by openBrTableJump (N > 2): the jump table's own base offset
 *  (lr's value once the dispatching BL runs), the address of the next
 *  not-yet-patched slot, and the one extra slot (index N, beyond every
 *  real case) for a genuinely out-of-range selector. present is false for
 *  an openBrTable (N ∈ {1,2}) frame, which has no table at all. */
struct TableInfo {
    bool present;
    uint32_t base;
    uint32_t nextFixupSlot;
    uint32_t endSlot;
};

struct Frame {
    FrameKind kind;
    uint32_t entryTos;

    // kind == Case
    uint32_t remaining;
    int32_t nextCaseFixup; // -1 = none
    TableInfo table;
    int32_t endFixupChain; // -1 = none; head of a backpatch chain (see closeBlockEnd's own comment)

    // kind == LoopCond / LoopBody
    uint32_t loopStart;
    int32_t exitFixup; // -1 = not yet set; meaningful only once kind == LoopBody
};

/** isa-core.md §16 item 5: bound the branch span guarded by a fused
 *  comparison *before* emitting it (a cheap, deliberately loose
 *  over-estimate), so the translator can choose a bare short-form
 *  conditional branch when that's provably safe and the invert-and-
 *  long-branch idiom otherwise — no separate fixup pass. from/bytesLen
 *  are the procedure's own raw wire bytes (decode_instr.h), from a byte
 *  offset into it. */
struct SpanResult { uint32_t bytes; uint32_t nextPc; };
SpanResult maxSpanBytes(const uint8_t *bytes, uint32_t bytesLen, uint32_t from, uint32_t blockCount);

/** Emit a branch that's taken exactly when condition holds, reaching
 *  blockCount sibling blocks starting at byte offset from — a bare
 *  conditional branch when maxSpanBytes proves that's in range, else the
 *  standard invert-and-long-branch idiom. Returns the site to hand to
 *  Emitter::patchBranch later; callers never need to know which shape
 *  they got (patchBranch/readBranchTarget already dispatch on the site's
 *  own encoding). */
uint32_t emitGuardedBranch(Emitter &e, ArmV6M::Condition condition, const uint8_t *bytes, uint32_t bytesLen, uint32_t from, uint32_t blockCount);

/** isa-core.md §7.1: acc < N executes case[acc]. Only the branch-fusion
 *  shape (N ∈ {1,2}) — condition is the *true* Thumb condition of
 *  whatever comparison immediately preceded this BR_TABLE (translate_proc
 *  .cpp is the one that knows that). bytes/bytesLen/pc are only for
 *  emitGuardedBranch's own span bound. */
Frame openBrTable(Emitter &e, Window &window, uint32_t n, ArmV6M::Condition condition, const uint8_t *bytes, uint32_t bytesLen, uint32_t pc);

/** isa-core.md §7.1 for N > 2: a genuine multi-way selector, dispatched
 *  via a shared per-procedure helper (emitBrTableHelper) instead of a
 *  fused conditional branch. helperSite is the placeholder BL site —
 *  translate_proc.cpp collects these across the whole procedure and
 *  patches them once emitBrTableHelper runs. */
struct OpenedJump { Frame frame; uint32_t helperSite; };
OpenedJump openBrTableJump(Emitter &e, Window &window, uint32_t n, AccState &accState);

/** isa-core.md §7.2: the condition sub-block about to be translated is
 *  compiled exactly once but reached via two different runtime paths
 *  (this call site's own fall-through, and the body's own back-edge) — so
 *  whatever it folds as an operand has to mean the same thing on both.
 *  flushLive forces both paths to arrive with accState in the identical
 *  state before either one reaches the condition's own first instruction. */
Frame openLoop(Emitter &e, Window &window, AccState &accState);

/** isa-core.md §7.1/§7.2/§8.1: closes the block a BLOCK_END instruction
 *  reaches. hasLoopExitCondition/loopExitCondition carry the fused (or
 *  synthesized via testAccNonzero) condition for a loopCond close — must
 *  be supplied exactly when frame.kind == LoopCond. Mutates frame in
 *  place (a case frame stays Case with remaining decremented, or
 *  transitions LoopCond -> LoopBody) and returns whether it's still open;
 *  false means this construct is fully closed and the caller should
 *  unwind its own recursive call for this nesting level. */
bool closeBlockEnd(Emitter &e, Window &window, AccState &accState, Frame &frame,
    bool hasLoopExitCondition, ArmV6M::Condition loopExitCondition,
    const uint8_t *bytes, uint32_t bytesLen, uint32_t pc);

/** isa-core.md §4.5/§7.1: a case frame's own close, when this case ends
 *  via a bare RETURN/TRAP instead of BLOCK_END (validated programs may
 *  shape it either way — packages/machine/src/validate.ts's own walk
 *  treats them identically for "where does the next sibling start").
 *  translate_proc.cpp's Frame bookkeeping has no free ride the way vm.ts's
 *  bare return/throw does: nextCaseFixup/the jump table's own next slot
 *  (and, on the *last* case, endFixupChain/the table's own end slot)
 *  still have to resolve to something. The reconciliation a *normal*
 *  close performs for the shared fall-through path (restoreWindow's real
 *  pop/sp-adjust, accState.flushLive's real materialize, a non-last
 *  case's "skip to end" branch) is moot here — the terminator's own
 *  emitted return sequence has already left the procedure — so only the
 *  bookkeeping half survives, as a plain state reset rather than emitted
 *  instructions nothing will ever execute. Returns whether more sibling
 *  cases remain (frame stays Case), same convention as closeBlockEnd. */
bool closeCaseViaTerminator(Emitter &e, Window &window, AccState &accState, Frame &frame);

/** closeCaseViaTerminator's own doc comment, but for a LOOP's body
 *  (isa-core.md §7.2's own explicit allowance). Unlike a case, there's no
 *  "more siblings" branch — a loop has exactly one body, so this always
 *  fully closes the construct; the only bookkeeping left is patching
 *  exitFixup (the condition's own cond-false exit branch) to land here. */
void closeLoopBodyViaTerminator(Emitter &e, Window &window, AccState &accState, Frame &frame);

/**
 * BR_TABLE N>2's shared per-procedure dispatch routine — reached by a
 * local BL from every openBrTableJump site in the same procedure, lr
 * pointing at that call site's own table. Returns the start pc, to patch
 * every collected helperSite against once the main body is fully
 * translated (dead code from a sequential-execution standpoint, emitted
 * once regardless of how many BR_TABLE N>2 sites the procedure has).
 */
uint32_t emitBrTableHelper(Emitter &e);

// ── Comparison → branch fusion (§10.1's "zero-destination" axis) ───────

/** Emit the CMP for a comparison whose *only* consumer is the following
 *  BR_TABLE/LOOP-condition BLOCK_END — never materializes a 0/1 result.
 *  Returns the Thumb condition that's true exactly when the comparison
 *  itself is true. operand == nullptr means PEEK_PEEK (not implemented —
 *  asserts, matching binops.h's own established gap for this combo). */
ArmV6M::Condition emitComparison(Emitter &e, AccState &accState, Op op, const Shape *operand);

/** isa-core.md §7.1/§7.2's own leniency: BR_TABLE/a LOOP condition's
 *  BLOCK_END test whatever value acc already holds, not specifically a
 *  comparison's 0/1 result — materialize whatever's pending, test it
 *  against zero explicitly, and hand back NE. */
ArmV6M::Condition testAccNonzero(Emitter &e, AccState &accState);

/** Materialize a comparison's boolean result (0 or 1) into dest — the
 *  general case emitComparison deliberately doesn't cover (docs/design.md
 *  §16 item 8: a comparison used as an ordinary value rather than a
 *  branch's own condition). */
void materializeComparison(Emitter &e, AccState &accState, Op op, const Shape *operand, uint32_t dest);

} // namespace jitc

#endif // JIT_ARMV6M_COMPILER_BLOCKS_H_
