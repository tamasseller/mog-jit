// The raw-memory test extension, reference half (docs/TODO.md's "implement
// raw memory access extension for testing").
//
// It exists to exercise the JIT's extension seam end to end rather than to
// be useful: a 1KB sandbox buffer, six load/store widths shaped the way the
// core's own ops are, and a MEMMOVE whose three operands all come off the
// operand stack — the one op here that touches acc not at all.
//
// Every offset is masked rather than bounds-checked, so there is no trap
// path for the two halves to disagree about, and MEMMOVE is specified as a
// forward byte copy so overlap is defined rather than divergent.
//
// MEMMOVE takes a destination *range*, not a length. Masking a length would
// have to be conservative enough that start + len still fit, costing half
// the address space, and would turn a length of exactly the buffer size
// into zero. Two masked endpoints are in range by construction, so the mask
// is simply the buffer size, and an end at or below its start copies
// nothing rather than something surprising.

import { PUSH, extInstr, pBuiltinCall, pRtl, rule, unaryNode, unaryTosNode } from "mog-core"
import type { Extension, ExtOpEffect } from "mog-core"
import type { ExtInstr, OutputLocation, Resource, Rule, RtlInstr, RtlNode } from "mog-core"

export const RAWMEM_BYTES = 1024
export const ADDR_MASK = 0x3ff // the whole buffer — see this file's header

/** Wire opcodes. Extension space is ≥128 (isa-core.md §5.1). */
export const RAWMEM_OPCODES: Readonly<Record<string, number>> = {
    LD8: 0x80,
    LD16: 0x81,
    LD32: 0x82,
    ST8: 0x83,
    ST16: 0x84,
    ST32: 0x85,
    MEMMOVE: 0x86,
    MEMCMP: 0x87,
    SLICECMP: 0x88,
}

const BY_BYTE = new Map<number, string>(
    Object.entries(RAWMEM_OPCODES).map(([name, code]) => [code, name]))

const WIDTH: Readonly<Record<string, number>> = {
    LD8: 1, LD16: 2, LD32: 4,
    ST8: 1, ST16: 2, ST32: 4,
}

/* Shaped like the core's own ops: a load is a unary transform on acc
 * (address in, value out, stack untouched), a store takes its value from
 * acc as STORE does and pops only the address. MEMMOVE is the deliberate
 * exception — three operands, all off the stack, none of them acc. */
const EFFECTS: Readonly<Record<string, ExtOpEffect>> = {
    LD8: {tosDelta: 0, maxTransient: 0, readsAcc: true, writesAcc: true},
    LD16: {tosDelta: 0, maxTransient: 0, readsAcc: true, writesAcc: true},
    LD32: {tosDelta: 0, maxTransient: 0, readsAcc: true, writesAcc: true},
    ST8: {tosDelta: -1, maxTransient: 0, readsAcc: true},
    ST16: {tosDelta: -1, maxTransient: 0, readsAcc: true},
    ST32: {tosDelta: -1, maxTransient: 0, readsAcc: true},
    /* killsAcc: the emitted code stages three operands into the helper's
     * argument registers, r0 among them, so acc is gone (isa-core.md
     * §11.2's third direction) — ext_rawmem.cpp's emitMemmove says the same
     * to the core by calling accInvalidate. */
    MEMMOVE: {tosDelta: -3, maxTransient: 0, killsAcc: true},
    MEMCMP: {tosDelta: -3, maxTransient: 0, writesAcc: true},
    /* maxTransient 1: the emitted code pushes the fourth operand for the
     * helper to read. Nothing native reserves that word — validate.ts folds
     * this into total_depth, which is what pays for it. */
    SLICECMP: {tosDelta: -4, maxTransient: 1, writesAcc: true},
}

export interface RawMemExtension extends Extension
{
    /** The sandbox itself, so a caller can seed it and compare it after. */
    readonly mem: Uint8Array
    /** Zeroed between programs — the target's buffer is static and outlives
     *  one program in a batch, so both halves have to start from the same
     *  state every time. */
    reset(): void
}

// ── DSL surface ─────────────────────────────────────────────────────────
//
// Without a `rules()` hook the extension is reachable only from hand-written
// RTL (make_seeds.ts's `extInstr`), so nothing starting from DSL source could
// emit an EXT op and the whole seam would be unreachable to an AST-level
// producer.
//
// Operand order is `exec`'s own read backwards — it pops in reverse, so a
// call's arguments push in source order. A store takes its address off the
// stack and its value from acc, matching `st`/`ld` in make_seeds.ts.

/** Compose N tiled arguments followed by one extension opcode — the
 *  multi-operand counterpart to rules.ts's `unaryNode`, which carries only
 *  one child's invariants. */
function seqNode(children: readonly RtlNode[], op: RtlInstr, demand: "acc" | "tos", effect: ExtOpEffect): RtlNode
{
    let running = 0
    let maxStack = 0
    for(const c of children)
    {
        maxStack = Math.max(maxStack, running + c.maxStack)
        running += c.tosDelta
    }
    maxStack = Math.max(maxStack, running + effect.maxTransient)

    const after = running + effect.tosDelta
    const fragment = [...children.flatMap(c => c.fragment), op]

    const clobbers = new Set<Resource>(children.flatMap(c => c.clobbers))
    if(demand === "tos") {clobbers.add("acc"); clobbers.delete("tos")}
    else clobbers.delete("acc")
    // An op that destroys acc says so even where `output` names it: the
    // output is declared only to satisfy orchestrator.ts's statement filter.
    if(effect.killsAcc) clobbers.add("acc")

    const output: OutputLocation[] = demand === "tos" ? ["tos"] : ["acc"]

    return demand === "tos"
        ? {type: "RtlNode", output, fragment: [...fragment, PUSH()], clobbers: [...clobbers],
           tosDelta: after + 1, maxStack: Math.max(maxStack, after + 1)}
        : {type: "RtlNode", output, fragment, clobbers: [...clobbers], tosDelta: after, maxStack}
}

/** Every argument of a stack-operand op, tiled to `"tos"`. */
const tosArgs = (m: {argumentMatches: readonly unknown[]}): RtlNode[] =>
    (m.argumentMatches as readonly {node: RtlNode}[]).map(a => a.node)

/** `Extension.rules` (extension.ts) — `resolveLocal`/`resolveCallee` are
 *  both unused: every operand here is an ordinary tiled value, never a
 *  named local or a callee reference. */
export function rawMemRules(): Rule[]
{
    const rules: Rule[] = []

    // A load is a unary transform on acc, exactly `clz`/`revbits`' shape.
    // The `:tos` variant is what lets one initialize a declaration.
    for(const name of ["LD8", "LD16", "LD32"] as const)
    {
        const dsl = name.toLowerCase()
        const body = (m: {argumentMatches: readonly {node: RtlNode}[]}): RtlInstr[] =>
            [...m.argumentMatches[0].node.fragment, extInstr(name, [])]

        rules.push(
            rule(`rawmem:${dsl}`, pBuiltinCall(dsl, pRtl("acc")), m =>
                unaryNode(m.argumentMatches[0].node, ["acc"], body(m))),
            rule(`rawmem:${dsl}:tos`, pBuiltinCall(dsl, pRtl("acc")), m =>
                unaryTosNode(m.argumentMatches[0].node, body(m))),
        )
    }

    // `st32(addr, value)` — address to the stack, value to acc, in that
    // order, which is the sequence `exec`'s own pop expects. A store keeps
    // acc (it only reads it), so its value is the value stored and both
    // demands have a form — without the `:tos` one a store nested in a
    // value position has no tiling at all.
    for(const name of ["ST8", "ST16", "ST32"] as const)
    {
        const dsl = name.toLowerCase()
        const build = (m: {argumentMatches: readonly {node: RtlNode}[]}, demand: "acc" | "tos"): RtlNode =>
            seqNode([m.argumentMatches[0]!.node, m.argumentMatches[1]!.node], extInstr(name, []), demand, EFFECTS[name])

        rules.push(
            rule(`rawmem:${dsl}`, pBuiltinCall(dsl, pRtl("tos"), pRtl("acc")), m => build(m, "acc")),
            rule(`rawmem:${dsl}:tos`, pBuiltinCall(dsl, pRtl("tos"), pRtl("acc")), m => build(m, "tos")),
        )
    }

    // Every operand off the stack: memmove(src, dstStart, dstEnd),
    // memcmp(aStart, aEnd, bStart), slicecmp(aStart, aEnd, bStart, bEnd).
    const STACK_ARITY: Readonly<Record<string, number>> = {MEMMOVE: 3, MEMCMP: 3, SLICECMP: 4}
    for(const [name, arity] of Object.entries(STACK_ARITY))
    {
        const dsl = name.toLowerCase()
        const args = Array.from({length: arity}, () => pRtl("tos"))

        rules.push(
            rule(`rawmem:${dsl}`, pBuiltinCall(dsl, ...args), m =>
                seqNode(tosArgs(m), extInstr(name, []), "acc", EFFECTS[name])),
        )

        // MEMMOVE gets no value-producing form: it declares `killsAcc`, so
        // there is nothing to read back and a use would fail validation.
        if(name !== "MEMMOVE")
        {
            rules.push(
                rule(`rawmem:${dsl}:tos`, pBuiltinCall(dsl, ...args), m =>
                    seqNode(tosArgs(m), extInstr(name, []), "tos", EFFECTS[name])),
            )
        }
    }

    return rules
}

export function rawMemExtension(): RawMemExtension
{
    const mem = new Uint8Array(RAWMEM_BYTES)

    /* Aligned down to the access width, not just masked: a Cortex-M0 faults
     * on an unaligned LDR/LDRH, so the emitted code has to align anyway and
     * this is what keeps the two halves agreeing on where it lands. */
    const at = (offset: number, width: number): number =>
        (offset & ADDR_MASK) & ~(width - 1)

    const load = (offset: number, width: number): number =>
    {
        const base = at(offset, width)
        let v = 0
        for(let i = 0; i < width; i++) v |= (mem[base + i] ?? 0) << (8 * i)
        return v >>> 0
    }

    const store = (offset: number, value: number, width: number): void =>
    {
        const base = at(offset, width)
        for(let i = 0; i < width; i++) mem[base + i] = (value >>> (8 * i)) & 0xff
    }

    return {
        mem,
        reset() {mem.fill(0)},
        effects: EFFECTS,
        rules: rawMemRules,
        exec(instr, state)
        {
            const name = instr.ext

            if(name === "MEMMOVE")
            {
                // Pop order mirrors the emitted code's own: dstEnd, then
                // dstStart, then src off the top.
                const dstEnd = state.pop() & ADDR_MASK
                const dstStart = state.pop() & ADDR_MASK
                let src = state.pop() & ADDR_MASK

                // Unsigned walk to the end, so a range whose end is at or
                // below its start copies nothing; the source wraps.
                for(let dst = dstStart; dst < dstEnd; dst++)
                {
                    mem[dst] = mem[src] ?? 0
                    src = (src + 1) & ADDR_MASK
                }

                return
            }

            if(name === "SLICECMP")
            {
                const bEnd = state.pop() & ADDR_MASK
                const bStart = state.pop() & ADDR_MASK
                const aEnd = state.pop() & ADDR_MASK
                const aStart = state.pop() & ADDR_MASK

                const lenA = aEnd > aStart ? aEnd - aStart : 0
                const lenB = bEnd > bStart ? bEnd - bStart : 0
                const common = Math.min(lenA, lenB)

                let result = lenA - lenB
                for(let i = 0; i < common; i++)
                {
                    const x = mem[aStart + i] ?? 0
                    const y = mem[bStart + i] ?? 0
                    if(x !== y) {result = x - y; break}
                }

                state.acc = result >>> 0
                return
            }

            if(name === "MEMCMP")
            {
                const bStart = state.pop() & ADDR_MASK
                const aEnd = state.pop() & ADDR_MASK
                const aStart = state.pop() & ADDR_MASK

                let b = bStart
                let result = 0

                for(let a = aStart; a < aEnd; a++)
                {
                    const x = mem[a] ?? 0
                    const y = mem[b] ?? 0
                    if(x !== y) {result = x - y; break}
                    b = (b + 1) & ADDR_MASK
                }

                state.acc = result >>> 0 // the first differing byte's difference
                return
            }

            const width = WIDTH[name]
            if(width === undefined) throw new Error(`rawmem: unknown op ${name}`)

            if(name.startsWith("LD"))
            {
                state.acc = load(state.acc, width)
            }
            else
            {
                store(state.pop(), state.acc, width)
            }
        },
        codec: {
            encode(instr: ExtInstr): number[]
            {
                const code = RAWMEM_OPCODES[instr.ext]
                if(code === undefined) throw new Error(`rawmem: cannot encode ${instr.ext}`)
                return [code]
            },
            decode(bytes: Uint8Array, offset: number): {instr: ExtInstr; next: number}
            {
                const name = BY_BYTE.get(bytes[offset] ?? -1)
                if(name === undefined) throw new Error(`rawmem: cannot decode byte ${bytes[offset]}`)
                return {instr: {op: "EXT", ext: name, operands: []}, next: offset + 1}
            },
        },
    }
}
