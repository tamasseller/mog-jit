/**
 * @ppl/jit-armv6m-prototype — lean bytecode reader (isa-core.md §5)
 *
 * This package's own copy of the opcode-byte-to-length table, deliberately
 * not `@ppl/machine`'s `decodeInstr` (packages/machine/src/bytecode.ts):
 * that one is generic over an extension's own payload shape (`E extends
 * {ext: string}`) and a registered `Extension`'s `codec`, machinery this
 * package never needs since `translateProc.ts` throws on `EXT` outright. A
 * no-heap native port needs this exact table again, in C++, regardless —
 * duplicating it here now is a faithful preview of that, not slack.
 *
 * Two granularities, two functions:
 *   - `readInstr` — `kind`/`imm` only, enough for a skip-pass
 *     (`procDirectory.ts`) that tracks block nesting and a handful of
 *     derived per-procedure facts but never inspects an arithmetic/
 *     comparison instruction's own operand.
 *   - `decodeInstr` (§16 item 16) — the same opcode table, but returns a
 *     full instruction shape (`DecodedInstr`, `RtlInstr`'s non-generic
 *     twin) `translateProc.ts`'s own main loop dispatches on directly,
 *     one instruction at a time from the raw byte stream, instead of a
 *     pre-decoded `RtlInstr[]` array walked by index.
 */

import type { BinaryOpcode, UnaryOpcode } from "@ppl/machine"

const ARITH_MODE_HAS_OPERAND = [true, true, false, false, true] // REG_ACC, REG_REG, PEEK_PEEK, POP_ACC, IMM_ACC
const CMP_MODE_HAS_OPERAND = [true, false, false, true] // REG_ACC, POP_ACC, IMM_ACC(#0), IMM_ACC

export const enum InstrKind { Plain, Call, BrTable, Loop, BlockEnd, Return, Trap, Ext, Unary }

export interface InstrInfo
{
    readonly kind: InstrKind
    /** Byte offset just past this instruction. */
    readonly next: number
    /** `BR_TABLE`'s own `N`, or (`kind === Unary`) `code - 90` — nothing
     *  else here carries an `imm`. */
    readonly imm?: number
}

/** Unsigned LEB128, this file's own copy of bytecode.ts's `decodeLeb128` —
 *  only the end offset is ever wanted here, never the decoded value,
 *  except for `BR_TABLE`'s `N` (savesLR's own predicate needs it) and the
 *  program-level procedure count/arg_count (procDirectory.ts). */
export function readLeb128(bytes: Uint8Array, offset: number): { value: number; next: number }
{
    let value = 0, shift = 0, pos = offset
    for (;;)
    {
        if (pos >= bytes.length)
            throw new Error(`readLeb128: ran off the end of the buffer at offset ${offset}`)
        const byte = bytes[pos]!
        value += (byte & 0x7f) * 2 ** shift
        pos++
        if ((byte & 0x80) === 0) break
        shift += 7
    }
    return { value: value >>> 0, next: pos }
}

/** Everything a skip-pass needs about the instruction at `offset`: which
 *  kind it is (if that matters to a caller), `BR_TABLE`'s own `N`, and
 *  where the next instruction starts. Never constructs an `RtlInstr`. */
export function readInstr(bytes: Uint8Array, offset: number): InstrInfo
{
    if (offset >= bytes.length)
        throw new Error(`readInstr: ran off the end of the buffer at offset ${offset}`)
    const code = bytes[offset]!
    const pos = offset + 1

    if (code >= 128)
        // isa-core.md §5.1/§11 — this reader's own extension point: a
        // registered extension would need to say how long its own opcode
        // is, the same way `Extension.codec` already does for full decode
        // (bytecode.ts). No such hook exists yet (future work, not yet
        // needed by anything this corpus exercises) — throw clearly
        // rather than guess a length.
        throw new Error(`readInstr: byte ${code} at offset ${offset} is an extension opcode — no length hook registered`)

    if (code <= 49)
    {
        const hasOperand = ARITH_MODE_HAS_OPERAND[code % 5]!
        return { kind: InstrKind.Plain, next: hasOperand ? readLeb128(bytes, pos).next : pos }
    }

    if (code <= 89)
    {
        const hasOperand = CMP_MODE_HAS_OPERAND[(code - 50) % 4]!
        return { kind: InstrKind.Plain, next: hasOperand ? readLeb128(bytes, pos).next : pos }
    }

    // unary (isa-core.md §4.3): code-90 selects NEG/NOT/CLZ/REVBITS —
    // carried as `imm` since procDirectory.ts's own savesLR needs to tell
    // CLZ/REVBITS (unaryops.ts's software helpers, reached by a local
    // `BL` — clobbers `lr` like any other nested call) apart from
    // NEG/NOT (single native instructions, no call at all).
    if (code <= 93) return { kind: InstrKind.Unary, next: pos, imm: code - 90 }

    switch (code)
    {
        case 94: return { kind: InstrKind.BlockEnd, next: pos }
        case 95: return { kind: InstrKind.Loop, next: pos }
        case 96: return { kind: InstrKind.BrTable, next: pos, imm: 1 }
        case 97: return { kind: InstrKind.BrTable, next: pos, imm: 2 }
        case 98: { const r = readLeb128(bytes, pos); return { kind: InstrKind.BrTable, next: r.next, imm: r.value } }
        case 99: return { kind: InstrKind.Call, next: readLeb128(bytes, pos).next }
        case 100: return { kind: InstrKind.Return, next: pos }
        case 101: return { kind: InstrKind.Trap, next: pos }
        case 102: return { kind: InstrKind.Trap, next: readLeb128(bytes, pos).next }
        case 103: case 104: return { kind: InstrKind.Plain, next: pos } // PUSH, POP
        case 105: case 106: case 107: return { kind: InstrKind.Plain, next: readLeb128(bytes, pos).next } // LOAD, STORE, CONST
    }

    if (code <= 123) return { kind: InstrKind.Plain, next: pos } // small CONST 0..15

    throw new Error(`readInstr: byte ${code} at offset ${offset} is reserved and unassigned (isa-core.md §5.3)`)
}

// ── Full per-instruction decode (§16 item 16) ───────────────────────────────
//
// `RtlInstr`'s own shape (packages/machine/src/rtl.ts), minus the `EXT`
// arm and its generic `E` payload — translateProc.ts never sees an `EXT`
// instruction without throwing first, so there is nothing here for one to
// carry. `ARITH_OPS`/`CMP_OPS`/`UNARY_OPS` mirror bytecode.ts's own tables
// exactly (isa-core.md §5.2/§5.3's opcode assignment), since this is the
// same wire format decoded a second time, not a different one.

export type DecodedInstr =
    | { op: BinaryOpcode; combo: "REG_ACC" | "REG_REG"; target: number }
    | { op: BinaryOpcode; combo: "IMM_ACC"; imm: number }
    | { op: BinaryOpcode; combo: "PEEK_PEEK" | "POP_ACC" }
    | { op: "LOAD" | "STORE"; target: number }
    | { op: "CONST"; imm: number }
    | { op: "NEG" | "NOT" | "CLZ" | "REVBITS" | "PUSH" | "POP" | "BLOCK_END" | "LOOP" | "RETURN" }
    | { op: "BR_TABLE" | "TRAP"; imm: number }
    | { op: "CALL"; calleeIndex: number }

const ARITH_OPS: readonly BinaryOpcode[] =
    ["ADD", "SUB", "RSUB", "MUL", "AND", "OR", "XOR", "SHL", "SHR", "ASR"]

const CMP_OPS: readonly BinaryOpcode[] =
    ["EQ", "NE", "LT_S", "LE_S", "GT_S", "GE_S", "LT_U", "LE_U", "GT_U", "GE_U"]

const UNARY_OPS: readonly UnaryOpcode[] = ["NEG", "NOT", "CLZ", "REVBITS"]

/** Full decode of the instruction at `offset` — `translateProc.ts`'s own
 *  main-loop primitive: every `body[pc]`/`pc++` it used to do against a
 *  pre-decoded `RtlInstr[]` becomes a call here plus `pc = result.next`
 *  instead (a fixed `+1` no longer makes sense once `pc` is a byte offset
 *  rather than an array index — different instructions are different
 *  lengths on the wire). */
export function decodeInstr(bytes: Uint8Array, offset: number): { instr: DecodedInstr; next: number }
{
    if (offset >= bytes.length)
        throw new Error(`decodeInstr: ran off the end of the buffer at offset ${offset}`)
    const code = bytes[offset]!
    const pos = offset + 1

    if (code >= 128)
        throw new Error(`decodeInstr: byte ${code} at offset ${offset} is an extension opcode — no decode hook registered`)

    if (code <= 49)
    {
        const op = ARITH_OPS[Math.floor(code / 5)]!
        const mode = code % 5
        if (mode === 0) { const r = readLeb128(bytes, pos); return { instr: { op, combo: "REG_ACC", target: r.value }, next: r.next } }
        if (mode === 1) { const r = readLeb128(bytes, pos); return { instr: { op, combo: "REG_REG", target: r.value }, next: r.next } }
        if (mode === 2) return { instr: { op, combo: "PEEK_PEEK" }, next: pos }
        if (mode === 3) return { instr: { op, combo: "POP_ACC" }, next: pos }
        const r = readLeb128(bytes, pos)
        return { instr: { op, combo: "IMM_ACC", imm: r.value }, next: r.next }
    }

    if (code <= 89)
    {
        const rel = code - 50
        const op = CMP_OPS[Math.floor(rel / 4)]!
        const mode = rel % 4
        if (mode === 0) { const r = readLeb128(bytes, pos); return { instr: { op, combo: "REG_ACC", target: r.value }, next: r.next } }
        if (mode === 1) return { instr: { op, combo: "POP_ACC" }, next: pos }
        if (mode === 2) return { instr: { op, combo: "IMM_ACC", imm: 0 }, next: pos }
        const r = readLeb128(bytes, pos)
        return { instr: { op, combo: "IMM_ACC", imm: r.value }, next: r.next }
    }

    if (code <= 93) return { instr: { op: UNARY_OPS[code - 90]! }, next: pos }

    switch (code)
    {
        case 94: return { instr: { op: "BLOCK_END" }, next: pos }
        case 95: return { instr: { op: "LOOP" }, next: pos }
        case 96: return { instr: { op: "BR_TABLE", imm: 1 }, next: pos }
        case 97: return { instr: { op: "BR_TABLE", imm: 2 }, next: pos }
        case 98: { const r = readLeb128(bytes, pos); return { instr: { op: "BR_TABLE", imm: r.value }, next: r.next } }
        case 99: { const r = readLeb128(bytes, pos); return { instr: { op: "CALL", calleeIndex: r.value }, next: r.next } }
        case 100: return { instr: { op: "RETURN" }, next: pos }
        case 101: return { instr: { op: "TRAP", imm: 0 }, next: pos }
        case 102: { const r = readLeb128(bytes, pos); return { instr: { op: "TRAP", imm: r.value }, next: r.next } }
        case 103: return { instr: { op: "PUSH" }, next: pos }
        case 104: return { instr: { op: "POP" }, next: pos }
        case 105: { const r = readLeb128(bytes, pos); return { instr: { op: "LOAD", target: r.value }, next: r.next } }
        case 106: { const r = readLeb128(bytes, pos); return { instr: { op: "STORE", target: r.value }, next: r.next } }
        case 107: { const r = readLeb128(bytes, pos); return { instr: { op: "CONST", imm: r.value }, next: r.next } }
    }

    if (code <= 123) return { instr: { op: "CONST", imm: code - 108 }, next: pos }

    throw new Error(`decodeInstr: byte ${code} at offset ${offset} is reserved and unassigned (isa-core.md §5.3)`)
}
