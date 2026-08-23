/**
 * @ppl/jit-armv6m-prototype — procedure directory (docs/design.md §9's own
 * dispatch table, but pointing at bytecode instead of compiled code)
 *
 * Built once, eagerly, at program entry, from one pass over the lumped
 * bytecode's own program framing (isa-core.md §5.5) — never re-walked per
 * `CALL`. Each entry records where that procedure's own body starts and
 * its `argCount`, straight from the header; `savesLR` (translateProc.ts's
 * own `needsLRSave` predicate — a `CALL`, a `BR_TABLE` with more than two
 * branches, or a `CLZ`/`REVBITS` unary op, anywhere in the body — every
 * one of them reached by a local `BL` that clobbers real hardware `lr`)
 * has no header field to read, so this pass derives it the only way
 * possible without a materialized `RtlInstr[]`: walking the body's own
 * bytes with bytecodeReader.ts.
 *
 * That same walk also has to find where the body itself ends (§5.5: no
 * stored length any more, bodies are self-delimiting — the wire-format
 * counterpart of this file's own reasoning lives in bytecode.ts's
 * `decodeProcBody`, reimplemented here rather than imported for the same
 * reason bytecodeReader.ts exists at all). Tracks open `LOOP`/`BR_TABLE`
 * frames by *kind*, not just a nesting count, because isa-core.md §7.2
 * lets a `LOOP`'s own body block close via a bare `RETURN`/`TRAP` instead
 * of `BLOCK_END` — that terminator pops its enclosing `loopBody` frame but
 * doesn't by itself end the procedure (mirrors blocks.ts's `Frame`
 * exactly, and vm.ts's `BlockFrame` before that).
 *
 * Extension point (future work, isa-core.md §11) — not built out, just not
 * foreclosed: a registered extension could grow its own per-procedure stat
 * in this same one pass, alongside `savesLR`, the same way `Extension.codec`
 * already hooks into wire decode (bytecode.ts) and `ExtOpEffect` into
 * whole-program validation (validate.ts).
 */

import { readInstr, readLeb128, InstrKind } from "./bytecodeReader"

export interface ProcDirEntry
{
    /** Byte offset of this procedure's own body, right after its
     *  `arg_count` — the pointer half of "pointer table." */
    readonly bytecodeOffset: number
    readonly argCount: number
    /** `translateProc.ts`'s `needsLRSave(proc)`, restated from raw bytes. */
    readonly savesLR: boolean
}

type ScanFrame = { kind: "case"; remaining: number } | { kind: "loopCond" } | { kind: "loopBody" }

/** One procedure's own self-delimited walk — see this file's header for
 *  why both halves (`next` and `savesLR`) fall out of the same pass. */
function scanProcBody(bytes: Uint8Array, offset: number): { next: number; savesLR: boolean }
{
    const stack: ScanFrame[] = []
    let pos = offset
    let savesLR = false

    for (;;)
    {
        const instr = readInstr(bytes, pos)
        if (instr.kind === InstrKind.Call) savesLR = true
        if (instr.kind === InstrKind.BrTable && instr.imm! > 2) savesLR = true
        if (instr.kind === InstrKind.Unary && instr.imm! >= 2) savesLR = true // CLZ (2) / REVBITS (3) — unaryops.ts's software helpers
        pos = instr.next

        if (instr.kind === InstrKind.BrTable) { stack.push({ kind: "case", remaining: instr.imm! }); continue }
        if (instr.kind === InstrKind.Loop) { stack.push({ kind: "loopCond" }); continue }

        if (instr.kind === InstrKind.BlockEnd)
        {
            const top = stack[stack.length - 1]
            if (!top) throw new Error(`scanProcBody: BLOCK_END with no open block at offset ${pos}`)
            if (top.kind === "case") { top.remaining -= 1; if (top.remaining === 0) stack.pop() }
            else if (top.kind === "loopCond") stack[stack.length - 1] = { kind: "loopBody" }
            else stack.pop() // loopBody's own ordinary (BLOCK_END) closer
            continue
        }

        if (instr.kind === InstrKind.Return || instr.kind === InstrKind.Trap)
        {
            // Empty stack *before* considering this terminator is the real
            // end (nothing open, nothing waiting on it as a closer). A
            // `loopBody` frame popping down to empty (§7.2) ends that
            // loop, not necessarily the procedure — the outer scope's own
            // bytes may still follow. A `case` frame (isa-core.md §4.5)
            // gets the same "does a terminator close me" treatment as
            // `BLOCK_END` a few lines up: closing this one case is not
            // necessarily the whole construct's own end if sibling cases
            // remain — those still follow right here in the byte stream,
            // so the frame stays open (with `remaining` advanced) instead
            // of popping. Either way, at most one frame pops per
            // terminator — an outer frame further down the stack, if any,
            // is untouched and closes later, via whatever its own
            // BLOCK_END/terminator turns out to be.
            if (stack.length === 0) return { next: pos, savesLR }
            const top = stack[stack.length - 1]!
            if (top.kind === "loopBody") stack.pop()
            else if (top.kind === "case") { top.remaining -= 1; if (top.remaining === 0) stack.pop() }
        }
    }
}

/** Builds the whole program's procedure directory from its lumped,
 *  program-framed bytecode (isa-core.md §5.5) — the skip-pass this file's
 *  header describes, run once at program entry. */
export function buildProcDirectory(bytes: Uint8Array): ProcDirEntry[]
{
    const countInfo = readLeb128(bytes, 0)
    const count = countInfo.value
    let pos = countInfo.next

    const dir: ProcDirEntry[] = []
    for (let i = 0; i < count; i++)
    {
        const argCountInfo = readLeb128(bytes, pos)
        const { next, savesLR } = scanProcBody(bytes, argCountInfo.next)
        dir.push({ bytecodeOffset: argCountInfo.next, argCount: argCountInfo.value, savesLR })
        pos = next
    }
    return dir
}
