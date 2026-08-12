/**
 * @ppl/jit-armv6m-prototype — ARMv6-M Thumb instruction encoder
 *
 * TS port of jit-armv6m/src/armv6.h (itself adapted from tamasseller/sdvm's
 * jit/armv6.h) — same bit layouts, same instruction set, so the two stay
 * eyeball-diffable. C++'s overload sets (`adds(d,n,imm)` vs `adds(d,n,m)`)
 * become distinct names here (`addsImm3`/`addsReg3`) since TS overload
 * resolution can't discriminate on argument *type* the way C++'s can — the
 * suffix names the operand shape, not a new instruction.
 *
 * Every function takes/returns plain `number`s (register indices 0..15,
 * immediates already shifted into their natural units) and returns one
 * encoded Thumb halfword. Range checks are runtime `assert`s, not
 * compile-time — this is a prototype for the *translation algorithm*, not
 * for the encoder's own type safety, which armv6.h already owns on the
 * real target.
 */

import assert from "node:assert/strict"

function assertRange(v: number, lo: number, hi: number, what: string): void
{
    assert.ok(Number.isInteger(v) && v >= lo && v <= hi, `${what}: ${v} out of range [${lo}, ${hi}]`)
}

function loReg(r: number): number { assertRange(r, 0, 7, "lo register"); return r }
function anyReg(r: number): number { assertRange(r, 0, 15, "register"); return r }
function imm(v: number, bits: number): number { assertRange(v, 0, (1 << bits) - 1, `imm${bits}`); return v }

// ── Reg2Op — Thumb-1 "2-operand" data-processing (destination is also an input) ──

enum Reg2Op
{
    AND = 0b0100000000_000_000, EOR = 0b0100000001_000_000,
    LSL = 0b0100000010_000_000, LSR = 0b0100000011_000_000,
    ASR = 0b0100000100_000_000, ADC = 0b0100000101_000_000,
    SBC = 0b0100000110_000_000, ROR = 0b0100000111_000_000,
    TST = 0b0100001000_000_000, RSB = 0b0100001001_000_000,
    CMP = 0b0100001010_000_000, CMN = 0b0100001011_000_000,
    ORR = 0b0100001100_000_000, MUL = 0b0100001101_000_000,
    BIC = 0b0100001110_000_000, MVN = 0b0100001111_000_000,
    SXH = 0b1011001000_000_000, SXB = 0b1011001001_000_000,
    UXH = 0b1011001010_000_000, UXB = 0b1011001011_000_000,
    REV = 0b1011101000_000_000, REV16 = 0b1011101001_000_000, REVSH = 0b1011101011_000_000,
}

function fmtReg2(op: Reg2Op, dn: number, m: number): number { return op | (m << 3) | dn }

export const ands  = (dn: number, m: number) => fmtReg2(Reg2Op.AND, loReg(dn), loReg(m))
export const eors  = (dn: number, m: number) => fmtReg2(Reg2Op.EOR, loReg(dn), loReg(m))
export const lslsReg = (dn: number, m: number) => fmtReg2(Reg2Op.LSL, loReg(dn), loReg(m))
export const lsrsReg = (dn: number, m: number) => fmtReg2(Reg2Op.LSR, loReg(dn), loReg(m))
export const asrsReg = (dn: number, m: number) => fmtReg2(Reg2Op.ASR, loReg(dn), loReg(m))
export const adcs  = (dn: number, m: number) => fmtReg2(Reg2Op.ADC, loReg(dn), loReg(m))
export const sbcs  = (dn: number, m: number) => fmtReg2(Reg2Op.SBC, loReg(dn), loReg(m))
export const rors  = (dn: number, m: number) => fmtReg2(Reg2Op.ROR, loReg(dn), loReg(m))
export const tst   = (n: number, m: number) => fmtReg2(Reg2Op.TST, loReg(n), loReg(m))
/** `d = 0 - m` (RSB with an implicit #0) — the native NEG idiom. */
export const negs  = (d: number, m: number) => fmtReg2(Reg2Op.RSB, loReg(d), loReg(m))
export const cmpReg = (n: number, m: number) => fmtReg2(Reg2Op.CMP, loReg(n), loReg(m))
export const cmn   = (n: number, m: number) => fmtReg2(Reg2Op.CMN, loReg(n), loReg(m))
export const orrs  = (dn: number, m: number) => fmtReg2(Reg2Op.ORR, loReg(dn), loReg(m))
export const muls  = (dn: number, m: number) => fmtReg2(Reg2Op.MUL, loReg(dn), loReg(m))
export const bics  = (dn: number, m: number) => fmtReg2(Reg2Op.BIC, loReg(dn), loReg(m))
export const mvns  = (dn: number, m: number) => fmtReg2(Reg2Op.MVN, loReg(dn), loReg(m))
export const sxth  = (d: number, m: number) => fmtReg2(Reg2Op.SXH, loReg(d), loReg(m))
export const sxtb  = (d: number, m: number) => fmtReg2(Reg2Op.SXB, loReg(d), loReg(m))
export const uxth  = (d: number, m: number) => fmtReg2(Reg2Op.UXH, loReg(d), loReg(m))
export const uxtb  = (d: number, m: number) => fmtReg2(Reg2Op.UXB, loReg(d), loReg(m))
export const rev   = (d: number, m: number) => fmtReg2(Reg2Op.REV, loReg(d), loReg(m))
export const rev16 = (d: number, m: number) => fmtReg2(Reg2Op.REV16, loReg(d), loReg(m))
export const revsh = (d: number, m: number) => fmtReg2(Reg2Op.REVSH, loReg(d), loReg(m))

// ── Reg3Op — 3-register / register+imm3 forms ──────────────────────────────

enum Reg3Op
{
    ADDREG = 0b0001100_000_000_000, SUBREG = 0b0001101_000_000_000,
    ADDIMM = 0b0001110_000_000_000, SUBIMM = 0b0001111_000_000_000,
    STR    = 0b0101000_000_000_000, STRH   = 0b0101001_000_000_000, STRB  = 0b0101010_000_000_000,
    LDRSB  = 0b0101011_000_000_000, LDR    = 0b0101100_000_000_000, LDRH  = 0b0101101_000_000_000,
    LDRB   = 0b0101110_000_000_000, LDRSH  = 0b0101111_000_000_000,
}

function fmtReg3(op: Reg3Op, dt: number, n: number, m: number): number { return op | (m << 6) | (n << 3) | dt }

export const addsReg3 = (d: number, n: number, m: number) => fmtReg3(Reg3Op.ADDREG, loReg(d), loReg(n), loReg(m))
export const subsReg3 = (d: number, n: number, m: number) => fmtReg3(Reg3Op.SUBREG, loReg(d), loReg(n), loReg(m))
export const addsImm3 = (d: number, n: number, i: number) => fmtReg3(Reg3Op.ADDIMM, loReg(d), loReg(n), imm(i, 3))
export const subsImm3 = (d: number, n: number, i: number) => fmtReg3(Reg3Op.SUBIMM, loReg(d), loReg(n), imm(i, 3))
export const str3   = (t: number, n: number, m: number) => fmtReg3(Reg3Op.STR, loReg(t), loReg(n), loReg(m))
export const strh3  = (t: number, n: number, m: number) => fmtReg3(Reg3Op.STRH, loReg(t), loReg(n), loReg(m))
export const strb3  = (t: number, n: number, m: number) => fmtReg3(Reg3Op.STRB, loReg(t), loReg(n), loReg(m))
export const ldrsb3 = (t: number, n: number, m: number) => fmtReg3(Reg3Op.LDRSB, loReg(t), loReg(n), loReg(m))
export const ldr3   = (t: number, n: number, m: number) => fmtReg3(Reg3Op.LDR, loReg(t), loReg(n), loReg(m))
export const ldrh3  = (t: number, n: number, m: number) => fmtReg3(Reg3Op.LDRH, loReg(t), loReg(n), loReg(m))
export const ldrb3  = (t: number, n: number, m: number) => fmtReg3(Reg3Op.LDRB, loReg(t), loReg(n), loReg(m))
export const ldrsh3 = (t: number, n: number, m: number) => fmtReg3(Reg3Op.LDRSH, loReg(t), loReg(n), loReg(m))

// ── Imm5Op — shift-by-immediate, register-offset load/store ────────────────

enum Imm5Op
{
    LSL = 0b00000_00000_000_000, LSR = 0b00001_00000_000_000, ASR = 0b00010_00000_000_000,
    STR = 0b01100_00000_000_000, LDR = 0b01101_00000_000_000,
    STRB = 0b01110_00000_000_000, LDRB = 0b01111_00000_000_000,
    STRH = 0b10000_00000_000_000, LDRH = 0b10001_00000_000_000,
}

function fmtImm5(op: Imm5Op, dt: number, mn: number, imm5: number): number { return op | (imm5 << 6) | (mn << 3) | dt }

export const lslsImm = (d: number, m: number, i: number) => fmtImm5(Imm5Op.LSL, loReg(d), loReg(m), imm(i, 5))
export const lsrsImm = (d: number, m: number, i: number) => fmtImm5(Imm5Op.LSR, loReg(d), loReg(m), imm(i, 5))
export const asrsImm = (d: number, m: number, i: number) => fmtImm5(Imm5Op.ASR, loReg(d), loReg(m), imm(i, 5))
/** `off` is a byte offset, must be a multiple of 4. */
export const str5  = (t: number, n: number, off: number) => fmtImm5(Imm5Op.STR, loReg(t), loReg(n), wordOff(off, 5))
export const ldr5  = (t: number, n: number, off: number) => fmtImm5(Imm5Op.LDR, loReg(t), loReg(n), wordOff(off, 5))
export const strh5 = (t: number, n: number, off: number) => fmtImm5(Imm5Op.STRH, loReg(t), loReg(n), halfOff(off, 5))
export const ldrh5 = (t: number, n: number, off: number) => fmtImm5(Imm5Op.LDRH, loReg(t), loReg(n), halfOff(off, 5))
export const strb5 = (t: number, n: number, off: number) => fmtImm5(Imm5Op.STRB, loReg(t), loReg(n), imm(off, 5))
export const ldrb5 = (t: number, n: number, off: number) => fmtImm5(Imm5Op.LDRB, loReg(t), loReg(n), imm(off, 5))

function wordOff(byteOff: number, bits: number): number { assert.ok(byteOff % 4 === 0, `word offset ${byteOff} not 4-aligned`); return imm(byteOff / 4, bits) }
function halfOff(byteOff: number, bits: number): number { assert.ok(byteOff % 2 === 0, `half offset ${byteOff} not 2-aligned`); return imm(byteOff / 2, bits) }

// ── Imm7Op — SP adjust ──────────────────────────────────────────────────────

enum Imm7Op { INCRSP = 0b101100000_0000000, DECRSP = 0b101100001_0000000 }
function fmtImm7(op: Imm7Op, imm7: number): number { return op | imm7 }
export const incrSp = (byteOff: number) => fmtImm7(Imm7Op.INCRSP, wordOff(byteOff, 7))
export const decrSp = (byteOff: number) => fmtImm7(Imm7Op.DECRSP, wordOff(byteOff, 7))

// ── Imm8Op — 8-bit-immediate / PC-or-SP-relative forms ──────────────────────

enum Imm8Op
{
    MOV = 0b00100_000_00000000, CMP = 0b00101_000_00000000,
    ADD = 0b00110_000_00000000, SUB = 0b00111_000_00000000,
    STRSP = 0b10010_000_00000000, LDRSP = 0b10011_000_00000000,
    LDR = 0b01001_000_00000000, ADR = 0b10100_000_00000000, ADDSP = 0b10101_000_00000000,
}

function fmtImm8(op: Imm8Op, r: number, imm8: number): number { return op | (r << 8) | imm8 }

export const movsImm8 = (d: number, i: number) => fmtImm8(Imm8Op.MOV, loReg(d), imm(i, 8))
export const cmpImm8  = (n: number, i: number) => fmtImm8(Imm8Op.CMP, loReg(n), imm(i, 8))
export const addsImm8 = (dn: number, i: number) => fmtImm8(Imm8Op.ADD, loReg(dn), imm(i, 8))
export const subsImm8 = (dn: number, i: number) => fmtImm8(Imm8Op.SUB, loReg(dn), imm(i, 8))
export const strSp = (t: number, byteOff: number) => fmtImm8(Imm8Op.STRSP, loReg(t), wordOff(byteOff, 8))
export const ldrSp = (t: number, byteOff: number) => fmtImm8(Imm8Op.LDRSP, loReg(t), wordOff(byteOff, 8))
/** PC-relative literal load — `byteOff` is measured from the *next*
 *  word-aligned address after this instruction (ARMv6-M's own rule), not
 *  from this instruction's own address; emit.ts's literal-pool helper owns
 *  computing that. */
export const ldrPc = (t: number, byteOff: number) => fmtImm8(Imm8Op.LDR, loReg(t), wordOff(byteOff, 8))
export const addPc = (d: number, byteOff: number) => fmtImm8(Imm8Op.ADR, loReg(d), wordOff(byteOff, 8))
export const addSp = (d: number, byteOff: number) => fmtImm8(Imm8Op.ADDSP, loReg(d), wordOff(byteOff, 8))

// ── Hi-register forms — the only three ops that can address r8-r15 ─────────

enum HiRegOp { ADD = 0b01000100_00000000, CMP = 0b01000101_00000000, MOV = 0b01000110_00000000, JMP = 0b01000111_00000000 }
function fmtHiReg(op: HiRegOp, dn: number, m: number): number { return op | ((dn >> 3) << 7) | (m << 3) | (dn & 0b0111) }

export const addHi = (dn: number, m: number) => fmtHiReg(HiRegOp.ADD, anyReg(dn), anyReg(m))
export const movHi = (dn: number, m: number) => fmtHiReg(HiRegOp.MOV, anyReg(dn), anyReg(m))
export const blx = (m: number) => fmtHiReg(HiRegOp.JMP, 0b1000, anyReg(m))
export const bx  = (m: number) => fmtHiReg(HiRegOp.JMP, 0b0000, anyReg(m))
export const cmpHi = (n: number, m: number) =>
{
    assert.ok((anyReg(n) & 0b1000) !== 0 || (anyReg(m) & 0b1000) !== 0, `cmpHi: at least one operand must be a hi register`)
    return fmtHiReg(HiRegOp.CMP, n, m)
}

// ── Push/pop, multi-register load/store ─────────────────────────────────────

function regFlags(regs: readonly number[]): number { return regs.reduce((f, r) => f | (1 << loReg(r)), 0) }
function fmtPushPop(pop: boolean, includeExtra: boolean, flags: number): number
{
    return 0b1011_0_10_0_00000000 | ((pop ? 1 : 0) << 11) | ((includeExtra ? 1 : 0) << 8) | flags
}

export const push = (regs: readonly number[]) => fmtPushPop(false, false, regFlags(regs))
export const pushWithLr = (regs: readonly number[]) => fmtPushPop(false, true, regFlags(regs))
export const pop = (regs: readonly number[]) => fmtPushPop(true, false, regFlags(regs))
export const popWithPc = (regs: readonly number[]) => fmtPushPop(true, true, regFlags(regs))

function lsMia(load: boolean, n: number, flags: number): number { return 0b11000_00000000000 | ((load ? 1 : 0) << 11) | (n << 8) | flags }
export const stmia = (n: number, regs: readonly number[]) => lsMia(false, loReg(n), regFlags(regs))
export const ldmia = (n: number, regs: readonly number[]) => lsMia(true, loReg(n), regFlags(regs))

// ── Conditional branches, SVC/UDF, unconditional branch ─────────────────────

export enum Condition
{
    EQ = 0b0000, NE = 0b0001, HS = 0b0010, LO = 0b0011,
    MI = 0b0100, PL = 0b0101, VS = 0b0110, VC = 0b0111,
    HI = 0b1000, LS = 0b1001, GE = 0b1010, LT = 0b1011,
    GT = 0b1100, LE = 0b1101,
}

export function inverse(c: Condition): Condition { return (c ^ 0b0001) as Condition }

/** `off` is a signed byte offset from this instruction's *own* address plus
 *  4 (ARMv6-M's own PC-relative convention), must be even; range
 *  ±256 bytes (8-bit signed imm, ×2). */
function ioff(off: number, aBits: number, nBits: number): number
{
    const scale = 1 << aBits
    assert.ok(off % scale === 0, `branch offset ${off} not ${scale}-aligned`)
    const min = -(1 << (nBits + aBits - 1)), max = (1 << (nBits + aBits - 1)) - 1
    assert.ok(off >= min && off <= max, `branch offset ${off} out of range [${min}, ${max}]`)
    return (off >> aBits) & ~(-1 << nBits)
}

export function isBranchOffsetInRange(off: number, aBits: number, nBits: number): boolean
{
    const scale = 1 << aBits
    if(off % scale !== 0) return false
    const min = -(1 << (nBits + aBits - 1)), max = (1 << (nBits + aBits - 1)) - 1
    return off >= min && off <= max
}

export const CBRANCH_A_BITS = 1, CBRANCH_N_BITS = 8
export const BRANCH_A_BITS = 1, BRANCH_N_BITS = 11

export function condBranch(c: Condition, off: number): number
{
    return (0b1101_0000_00000000 | (c << 8)) | ioff(off, CBRANCH_A_BITS, CBRANCH_N_BITS)
}

export const udf = (code: number) => 0b1101_1110_00000000 | imm(code, 8)
export const svc = (code: number) => 0b1101_1111_00000000 | imm(code, 8)
export const b = (off: number) => 0b11100_00000000000 | ioff(off, BRANCH_A_BITS, BRANCH_N_BITS)
export const bkpt = (code: number) => 0b10111110_00000000 | imm(code, 8)

export const nop = () => 0b1011_1111_0000_0000

/**
 * Materialize an arbitrary 32-bit constant into `dst`, MSB-first byte
 * chunks (`MOVS` for the first nonzero byte, then `LSLS #8; ADDS` per
 * remaining byte, skipping an `ADDS` for a zero byte since the shift
 * already leaves that position zeroed) — up to 7 instructions, fewer for
 * small values. No literal pool needed: Thumb-1 baseline has no
 * single-instruction 32-bit immediate load, but this needs nothing beyond
 * what CONST-ing any value already requires, so a per-procedure literal
 * pool (§11's own PC-relative-load discussion, for a *different* problem —
 * reaching a fixed helper address) isn't worth the bookkeeping here.
 */
export function synthesizeImm32(dst: number, value: number): number[]
{
    const v = value >>> 0
    const bytes = [(v >>> 24) & 0xff, (v >>> 16) & 0xff, (v >>> 8) & 0xff, v & 0xff]
    let start = 0
    while(start < 3 && bytes[start] === 0) start++

    const out = [movsImm8(dst, bytes[start]!)]
    for(let i = start + 1; i < 4; i++)
    {
        out.push(lslsImm(dst, dst, 8))
        if(bytes[i] !== 0) out.push(addsImm8(dst, bytes[i]!))
    }
    return out
}

export function fitsImm8(v: number): boolean { return Number.isInteger(v) && v >= 0 && v <= 0xff }
export function fitsImm3(v: number): boolean { return Number.isInteger(v) && v >= 0 && v <= 0x7 }

// ── Fixup introspection — read/patch an already-emitted instruction's
//    displacement in place, for the backpatch scheme in emit.ts/blocks.ts ──

export function isCondBranch(isn: number): boolean { return (isn >> 12) === 0b1101 && ((isn >> 8) & 0b1111) < 0b1101 }
export function getCondBranchOffset(isn: number): number
{
    assert.ok(isCondBranch(isn), `not a conditional branch: 0x${isn.toString(16)}`)
    const raw = isn & 0xff
    return ((raw << 24) >> 24) << CBRANCH_A_BITS // sign-extend the 8-bit field, then re-scale
}
export function setCondBranchOffset(isn: number, off: number): number
{
    assert.ok(isCondBranch(isn), `not a conditional branch: 0x${isn.toString(16)}`)
    return (isn & ~0xff) | ioff(off, CBRANCH_A_BITS, CBRANCH_N_BITS)
}

export function isUncondBranch(isn: number): boolean { return (isn >>> 11) === 0b11100 }
export function getUncondBranchOffset(isn: number): number
{
    assert.ok(isUncondBranch(isn), `not an unconditional branch: 0x${isn.toString(16)}`)
    const raw = isn & 0x7ff
    return ((raw << 21) >> 21) << BRANCH_A_BITS
}
export function setUncondBranchOffset(isn: number, off: number): number
{
    assert.ok(isUncondBranch(isn), `not an unconditional branch: 0x${isn.toString(16)}`)
    return (isn & ~0x7ff) | ioff(off, BRANCH_A_BITS, BRANCH_N_BITS)
}
