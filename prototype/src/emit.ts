/**
 * @ppl/jit-armv6m-prototype — code buffer + branch backpatching
 *
 * Deliberately dumb: knows nothing about the Generic Core ISA or the
 * translation state machines — just "append a halfword, remember where a
 * branch's target isn't known yet, patch it once it is." blocks.ts is the
 * only caller that decides *when* a target becomes known and issues the
 * patch; this module just makes that a one-line call instead of hand
 * re-deriving §11's PC-relative-displacement arithmetic at every call site.
 *
 * docs/jit-armv6m.md §16 item 1 flagged whether translation genuinely needs
 * Pass 2 (a separate fixup pass over the whole procedure) as still open.
 * `patchBranch` being usable the instant a block closes — never needing to
 * look at instructions emitted after the site being patched — is the
 * concrete evidence for "no, not for branch targets": every branch this
 * translator ever emits closes over a target that's already known by the
 * time its enclosing block ends (isa-core.md §7.1/§7.2's own block-nesting
 * discipline guarantees that), so `blocks.ts` never holds a fixup open past
 * one `BLOCK_END`/loop-back-edge. What's *not* covered here, and still
 * plausibly needs its own pass: out-of-range branches needing the
 * invert-and-long-branch idiom, and `BR_TABLE N>2` jump tables — neither
 * implemented yet (translateProc.ts).
 */

import * as arm from "./armv6"

export class Emitter
{
    private readonly halfwords: number[] = []

    get pc(): number { return this.halfwords.length * 2 }

    /** Append one already-encoded halfword; returns its own byte offset. */
    emit(word: number): number
    {
        const at = this.pc
        this.halfwords.push(word & 0xffff)
        return at
    }

    /** Emit a conditional branch with a placeholder (zero) offset — the
     *  common case where the target is a forward reference not yet known.
     *  Returns the site's byte offset, to hand to `patchBranch` later. */
    placeholderCondBranch(c: arm.Condition): number
    {
        return this.emit(arm.condBranch(c, 0))
    }

    /** Same, for an unconditional branch. */
    placeholderBranch(): number
    {
        return this.emit(arm.b(0))
    }

    /** A placeholder `BL` (two halfwords) for a `CALL` site — the target
     *  procedure's own final offset isn't known until program.ts has laid
     *  out every procedure, well after this one's own translation is done,
     *  so this only ever gets patched by poking the combined program's
     *  buffer directly (program.ts), never via `patchBranch` above (that's
     *  strictly `blocks.ts`'s single-procedure-local scheme). Returns the
     *  site's own byte offset, local to this procedure. */
    placeholderBL(): number
    {
        const at = this.pc
        const [hw1, hw2] = arm.bl(0)
        this.emit(hw1)
        this.emit(hw2)
        return at
    }

    /** Resolve a previously-emitted (conditional or unconditional) branch's
     *  target now that it's known — never needs to have seen anything past
     *  `siteOffset` at the time it was placed (see this file's header). */
    patchBranch(siteOffset: number, targetOffset: number): void
    {
        const idx = siteOffset / 2
        const isn = this.halfwords[idx]!
        const delta = targetOffset - (siteOffset + 4)
        this.halfwords[idx] = arm.isCondBranch(isn)
            ? arm.setCondBranchOffset(isn, delta)
            : arm.setUncondBranchOffset(isn, delta)
    }

    /** Overwrite a single already-emitted halfword with an arbitrary raw
     *  value — a `BR_TABLE N>2` jump-table slot (blocks.ts's
     *  `openBrTableJump`), not an instruction, so there's no encoding to
     *  preserve the way `patchBranch` above has to. */
    patchLiteral(siteOffset: number, value: number): void
    {
        this.halfwords[siteOffset / 2] = value & 0xffff
    }

    /** Resolve a `placeholderBL` site once its target is known *within
     *  this same procedure* — blocks.ts's `BR_TABLE N>2` helper is emitted
     *  once per procedure and reached by a local `BL`, unlike `CALL`'s own
     *  cross-procedure one (program.ts, never through here). */
    patchBL(siteOffset: number, targetOffset: number): void
    {
        const [hw1, hw2] = arm.bl(targetOffset - (siteOffset + 4))
        this.halfwords[siteOffset / 2] = hw1
        this.halfwords[siteOffset / 2 + 1] = hw2
    }

    toUint16Array(): Uint16Array
    {
        return Uint16Array.from(this.halfwords)
    }
}
