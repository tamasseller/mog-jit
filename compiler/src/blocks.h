// jit-armv6m/compiler — block structure (isa-core.md §7.1/§7.2). Nesting is
// native recursion in translate_proc.cpp (one call per open LOOP/BR_TABLE,
// this file's own Frame held as a plain local), not an explicit stack — the
// property that matters for a no-heap port is "bounded by nesting depth,"
// and a real call stack is already bounded and already there.
//
// Frame is a flat, aggregate-initializable struct with a Kind tag rather
// than a tagged union (instr.h's own header explains the same choice for
// Instr) — the case/loopCond/loopBody shapes share this file's own
// handful of fields, and a plain struct needs no manual lifetime
// management to live as a stack local across translate_proc.cpp's own
// recursive calls. -1 is the "none" sentinel for an int32_t fixup site,
// since 0 is a valid real site offset (the procedure's own first byte).
//
// Every branch this translator emits resolves its target by the time its
// *own* enclosing BLOCK_END/back-edge is reached — a LOOP back-edge target
// is already known the instant LOOP opens (§7.2's own structure — the
// condition block starts right there), and a BR_TABLE case's "skip to next
// case"/"skip to end" targets resolve the moment that case (or the whole
// construct) closes. No separate pass over the whole procedure is needed
// for this — so a long-branch conditional's out-of-range concern
// (emitGuardedBranch, below) is handled inline, at the one or two sites
// that ever emit a fused conditional branch, by bounding the guarded span
// *before* emitting it (a cheap, deliberately loose over-estimate,
// maxSpanBytes) rather than by adding a genuine second pass.
#ifndef JIT_ARMV6M_COMPILER_BLOCKS_H_
#define JIT_ARMV6M_COMPILER_BLOCKS_H_

#include <cstdint>
#include "instr.h"
#include "shape.h"
#include "armv6.h"

namespace jitc
{

class Emitter;
class Window;
class AccState;

enum class FrameKind : uint8_t
{
    Case,
    LoopCond,
    LoopBody
};

/** Set only by openBrTableJump (N > 2): the jump table's own base offset
 *  (lr's value once the dispatching BL runs), the address of the next
 *  not-yet-patched slot, and the one extra slot (index N, beyond every
 *  real case) for a genuinely out-of-range selector. present is false for
 *  an openBrTable (N ∈ {1,2}) frame, which has no table at all. */
struct TableInfo
{
    bool present;
    uint32_t base;
    uint32_t nextFixupSlot;
    uint32_t endSlot;
};

struct Frame
{
    FrameKind kind;
    uint32_t entryTos;

    // kind == Case
    uint32_t remaining;
    int32_t nextCaseFixup; // -1 = none
    TableInfo table;
    int32_t endFixupChain; // -1 = none; head of a backpatch chain (see closeBlockEnd's own comment)
    // Set only by openBrTable (never openBrTableJump, which has a real
    // multi-way value, not a boolean): case[1], if this frame has one, is
    // reached with no register anywhere holding acc's own value — the
    // fused branch only ever set CPU flags — so closeBlockEnd's own case
    // branch uses this to seed accState with the statically-known value
    // (case[1] runs exactly when the fused comparison was true) instead of
    // silently leaving whatever accState described *before* the
    // comparison ran.
    bool fusedBoolean;

    // kind == LoopCond / LoopBody
    uint32_t loopStart;
    int32_t exitFixup; // -1 = not yet set; meaningful only once kind == LoopBody
};

/** Bound the branch span guarded by a fused comparison *before* emitting
 *  it (a cheap, deliberately loose over-estimate), so the translator can
 *  choose a bare short-form conditional branch when that's provably safe
 *  and the invert-and-long-branch idiom otherwise — no separate fixup
 *  pass. from/bytesLen are the procedure's own raw wire bytes
 *  (decode_instr.h), from a byte offset into it. */
struct SpanResult
{
    uint32_t bytes;
    uint32_t nextPc;
};
SpanResult maxSpanBytes(const uint8_t *bytes, uint32_t bytesLen, uint32_t from, uint32_t blockCount);

/** Emit a branch that's taken exactly when condition holds, reaching
 *  blockCount sibling blocks starting at byte offset from — a bare
 *  conditional branch when maxSpanBytes proves that's in range, else the
 *  standard invert-and-long-branch idiom. Returns the site to hand to
 *  Emitter::patchBranch later; callers never need to know which shape
 *  they got (patchBranch/readBranchTarget already dispatch on the site's
 *  own encoding).
 *
 *  pendingPoolBytes is what translate_proc.cpp's literal pool still owes
 *  the output stream (translate_proc.cpp's own literalPoolDebt) — bytes
 *  that maxSpanBytes cannot see, since it walks bytecode alone, but that a
 *  flush may well drop inside this very span. Sites *added* within the
 *  span need no extra budget: each costs 2 bytes of placeholder plus 4 of
 *  pool word against the ORDINARY_MAX_BYTES already priced for its own
 *  instruction. */
uint32_t emitGuardedBranch(Emitter &e, ArmV6M::Condition condition, const uint8_t *bytes, uint32_t bytesLen, uint32_t from, uint32_t blockCount, uint32_t pendingPoolBytes);

/** isa-core.md §7.1: acc < N executes case[acc]. Only the branch-fusion
 *  shape (N ∈ {1,2}) — condition is the *true* Thumb condition of
 *  whatever comparison immediately preceded this BR_TABLE (translate_proc.cpp
 *  is the one that knows that); fused is whether that comparison was
 *  genuinely there (vs. condition coming from testAccNonzero, where acc's
 *  real, unreplaced value stays correct on both branches with nothing to
 *  seed). When fused, case[0] (this frame's own entry) starts with
 *  accState already set to reflect the comparison's known-false result —
 *  see Frame::fusedBoolean for case[1]'s own half of this. bytes/bytesLen
 *  /pc/pendingPoolBytes are only for emitGuardedBranch's own span bound. */
Frame openBrTable(Emitter &e, Window &window, AccState &accState, uint32_t n, ArmV6M::Condition condition, bool fused, const uint8_t *bytes, uint32_t bytesLen, uint32_t pc, uint32_t pendingPoolBytes);

/** isa-core.md §7.1 for N > 2: a genuine multi-way selector, dispatched via
 *  the flash-resident brTableJumpHelper (docs/design.md §11's reserved slot
 *  6, jit-armv6m/runtime/runtime.S) instead of a fused conditional branch.
 *  Reached by BLX, not a local BL: lr needs to end up pointing at this call
 *  site's own jump table (emitted immediately after), exactly as it would
 *  for a local BL — brTableJumpHelper reads it the same way either way. */
Frame openBrTableJump(Emitter &e, Window &window, uint32_t n, AccState &accState);

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
 *  be supplied exactly when frame.kind == LoopCond; fusedLoopExit (only
 *  meaningful alongside it) is whether that condition came from a real
 *  fused comparison rather than testAccNonzero — exactly like
 *  Frame::fusedBoolean's own gate, no register holds the loop body's own
 *  "keep looping" value when true, so this seeds accState with the known
 *  constant (comparison was true) instead of leaving the pre-comparison
 *  value in place. Mutates frame in place (a case frame stays Case with
 *  remaining decremented, or transitions LoopCond -> LoopBody) and returns
 *  whether it's still open; false means this construct is fully closed and
 *  the caller should unwind its own recursive call for this nesting
 *  level. */
bool closeBlockEnd(Emitter &e, Window &window, AccState &accState, Frame &frame,
    bool hasLoopExitCondition, ArmV6M::Condition loopExitCondition, bool fusedLoopExit,
    const uint8_t *bytes, uint32_t bytesLen, uint32_t pc, uint32_t pendingPoolBytes);

/** isa-core.md §4.5/§7.1: a case frame's own close, when this case ends via
 *  a bare RETURN/TRAP instead of BLOCK_END (validated programs may shape it
 *  either way — packages/machine/src/validate.ts treats them identically
 *  for "where does the next sibling start"). The forward-branch bookkeeping
 *  (nextCaseFixup/the jump table's own next slot, and — on the *last* case
 *  — endFixupChain/the table's own end slot) still has to resolve to
 *  something even though the terminator's own emitted return sequence has
 *  already left the procedure — so only that bookkeeping survives here, as
 *  a plain state reset rather than emitted instructions nothing will ever
 *  execute. Returns whether more sibling cases remain (frame stays Case),
 *  same convention as closeBlockEnd. */
bool closeCaseViaTerminator(Emitter &e, Window &window, AccState &accState, Frame &frame);

/** closeCaseViaTerminator's own doc comment, but for a LOOP's body
 *  (isa-core.md §7.2's own explicit allowance). Unlike a case, there's no
 *  "more siblings" branch — a loop has exactly one body, so this always
 *  fully closes the construct; the only bookkeeping left is patching
 *  exitFixup (the condition's own cond-false exit branch) to land here. */
void closeLoopBodyViaTerminator(Emitter &e, Window &window, AccState &accState, Frame &frame);

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
 *  general case emitComparison deliberately doesn't cover: a comparison
 *  used as an ordinary value rather than a branch's own condition. */
void materializeComparison(Emitter &e, AccState &accState, Op op, const Shape *operand, uint32_t dest);

} // namespace jitc

#endif // JIT_ARMV6M_COMPILER_BLOCKS_H_
