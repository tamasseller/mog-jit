// The sample-stream extension, reference half — the mirror of
// bench/ext_sampstream.{h,cpp}, which this file specifies.
//
// It is the I/O surface the benchmark workloads are written against: a
// window of input samples, an output stream for filters, and a trigger-event
// sink. Three ops, all of which the target emits inline, because the whole
// point of the suite is comparing JIT-emitted Thumb against C doing the same
// work — an op that reached its data through a helper call would measure the
// extension seam instead.
//
// The index lives in the *program*, not here. A stateful `next_sample()`
// would have to keep its cursor in extension memory, costing a pooled base
// plus a load and a store on every access, where C keeps the same cursor in
// a register across the whole loop. An indexed ring gives the JIT's own
// register allocator the cursor as an ordinary local, and look-back is then
// `sample_at(i - n)` with no second op.
//
// Offsets are masked rather than bounds-checked, exactly as in
// fuzz/rawmem_ext.ts: the mask *is* the buffer size, so there is no trap
// path for the two halves to disagree about.

import type {Extension, ExtOpEffect, ExtInstr, Rule, RtlNode} from "../../packages/machine/src/index"
import {rule, pBuiltinCall, pConst, pRtl, unaryNode, PUSH,
    encodeLeb128, decodeLeb128} from "../../packages/machine/src/index"

/* Sizes are powers of two: the mask is the size, and masking is two shifts
 * on a core with no AND-immediate. Mirrored in ext_sampstream.h. */
export const SAMP_IN_SAMPLES = 4096
export const SAMP_OUT_SAMPLES = 1024
export const SAMP_EVENTS = 32

export const IN_MASK = SAMP_IN_SAMPLES - 1
export const OUT_MASK = SAMP_OUT_SAMPLES - 1
export const EVENT_MASK = SAMP_EVENTS - 1

/** Wire opcodes. Extension space is >=128 (isa-core.md §5.1). */
export const SAMPSTREAM_OPCODES: Readonly<Record<string, number>> = {
    SAMPLE_AT: 0x80,
    OUT_AT: 0x81,
    TRIGGER: 0x82,
}

const BY_BYTE = new Map<number, string>(
    Object.entries(SAMPSTREAM_OPCODES).map(([name, code]) => [code, name]))

/* A trigger event is one word: the sample index in the top 28 bits and the
 * kind in the low 4. Packing rather than two parallel arrays keeps the
 * emitted sequence to a single store, and a benchmark never runs long
 * enough for an index to reach 2^28. */
export const TRIGGER_KIND_BITS = 4
export const TRIGGER_MAX_KIND = (1 << TRIGGER_KIND_BITS) - 1

export const packEvent = (index: number, kind: number): number =>
    (((index << TRIGGER_KIND_BITS) | (kind & TRIGGER_MAX_KIND)) >>> 0)

/* SAMPLE_AT is a unary transform on acc, shaped like the core's own unary
 * ops; OUT_AT mirrors STORE, taking its value from acc and its index off
 * the stack; TRIGGER reads the index from acc and leaves it there, so it
 * declares no writesAcc and the emitted code must not touch r0. */
const EFFECTS: Readonly<Record<string, ExtOpEffect>> = {
    SAMPLE_AT: {tosDelta: 0, maxTransient: 0, readsAcc: true, writesAcc: true},
    OUT_AT: {tosDelta: -1, maxTransient: 0, readsAcc: true},
    TRIGGER: {tosDelta: 0, maxTransient: 0, readsAcc: true},
}

const sampleAtInstr = (): ExtInstr => ({op: "EXT", ext: "SAMPLE_AT", operands: []})
const outAtInstr = (): ExtInstr => ({op: "EXT", ext: "OUT_AT", operands: []})
const triggerInstr = (kind: number): ExtInstr => ({op: "EXT", ext: "TRIGGER", operands: [kind]})

/* Sequential composition of an operand list, the same walk nodeInvariants
 * (builders.ts) does for a core combo's children — each child's own peak
 * measured from the depth its predecessors left behind, and the op's own
 * clobbers minus whatever the output occupies. Extension ops have no
 * ComboName, so nodeInvariants itself is not reachable here.
 *
 * `opClobbers` is the load-bearing argument. An op that writes acc and is
 * asked for `"tos"` output leaves acc destroyed, and a node that fails to
 * say so lets the orchestrator schedule an acc-resident sibling ahead of
 * it: `sample_at(i) + sample_at(i - 1)` then evaluates the left operand
 * into acc, overwrites it computing the right, and adds the right to
 * itself. */
function seqNode(children: RtlNode[], output: ("acc" | "tos")[],
    fragment: RtlNode["fragment"], opTosDelta: number, opClobbers: ("acc" | "tos")[] = [],
    extraTosDelta = 0, extraMaxStack?: number): RtlNode
{
    let running = 0
    let maxStack = 0

    for(const c of children)
    {
        maxStack = Math.max(maxStack, running + c.maxStack)
        running += c.tosDelta
    }

    running += opTosDelta

    if(extraMaxStack !== undefined)
    {
        maxStack = Math.max(maxStack, running + extraMaxStack)
    }

    const clobbers = new Set([...children.flatMap(c => c.clobbers), ...opClobbers])
    for(const loc of output) clobbers.delete(loc)

    return {
        type: "RtlNode",
        output,
        fragment,
        clobbers: [...clobbers],
        tosDelta: children.reduce((s, c) => s + c.tosDelta, 0) + opTosDelta + extraTosDelta,
        maxStack,
    }
}

/**
 * Every value-producing rule needs both an `:acc` and a `:tos` variant.
 * There is no automatic adapter between the two output locations — every
 * core rule spells out the pair (rules.ts's `literal:acc`/`literal:tos`,
 * `immOperandRules`' variants) — and without the `:tos` half a call could
 * never be a binary operand, since `pBinary`'s two children demand one of
 * each. That is exactly the gap that forces every codec body into the
 * `u32 x = 0; x = load_val(1);` two-step, and DSP source cannot afford it:
 * `sample_at(i) - sample_at(i - 1)` has to tile directly.
 */
export function sampStreamRules(): Rule[]
{
    const rules: Rule[] = []

    for(const loc of ["acc", "tos"] as const)
    {
        const push = loc === "tos" ? [PUSH()] : []
        const extraTos = loc === "tos" ? 1 : 0
        const extraMax = loc === "tos" ? 1 : undefined

        rules.push(rule(`sampstream:sample_at:${loc}`, pBuiltinCall("sample_at", pRtl("acc")), m =>
        {
            const [index] = m.argumentMatches
            return seqNode([index.node], [loc],
                [...index.node.fragment, sampleAtInstr(), ...push], 0, ["acc"], extraTos, extraMax)
        }))
    }

    /* Value in acc, index on the stack: OUT_AT pops the index, so the index
     * is tiled first and the value second, and the pair nets zero. acc is
     * left holding the value written, which is what the "acc" output
     * claims — OUT_AT declares no writesAcc and the target must not touch
     * r0 either. */
    rules.push(rule("sampstream:out_at", pBuiltinCall("out_at", pRtl("tos"), pRtl("acc")), m =>
    {
        const [index, value] = m.argumentMatches
        return seqNode([index.node, value.node], ["acc"],
            [...index.node.fragment, ...value.node.fragment, outAtInstr()], -1)
    }))

    /* The kind is always a codegen-time literal, so it rides the wire as an
     * operand rather than through acc; the index is the dynamic half. */
    rules.push(rule("sampstream:trigger", pBuiltinCall("trigger", pConst(), pRtl("acc")), m =>
    {
        const [kind, index] = m.argumentMatches

        if(kind.value < 0 || kind.value > TRIGGER_MAX_KIND) return undefined

        return unaryNode(index.node, ["acc"],
            [...index.node.fragment, triggerInstr(kind.value)])
    }))

    return rules
}

export interface SampStreamExtension extends Extension
{
    /** The input window. Read-only to a program; the harness fills it. */
    readonly input: Int16Array
    readonly output: Int16Array
    /** Packed events, `packEvent`-encoded, and how many were emitted —
     *  the count is not masked, so an overrun is visible rather than
     *  silently indistinguishable from a full ring. */
    readonly events: Uint32Array
    eventCount(): number
    /** Zeroed between programs: the target's buffers are static and outlive
     *  one program, the reference VM's do not. */
    reset(): void
}

export function sampStreamExtension(): SampStreamExtension
{
    const input = new Int16Array(SAMP_IN_SAMPLES)
    const output = new Int16Array(SAMP_OUT_SAMPLES)
    const events = new Uint32Array(SAMP_EVENTS)
    let count = 0

    return {
        input,
        output,
        events,
        eventCount() {return count},
        reset()
        {
            output.fill(0)
            events.fill(0)
            count = 0
        },
        rules: () => sampStreamRules(),
        effects: EFFECTS,
        exec(instr, state)
        {
            switch(instr.ext)
            {
                case "SAMPLE_AT":
                    // Int16Array's own read sign-extends, matching LDRSH.
                    state.acc = input[state.acc & IN_MASK]! >>> 0
                    return

                case "OUT_AT":
                    // Int16Array's own write truncates, matching STRH.
                    output[state.pop() & OUT_MASK] = state.acc | 0
                    return

                case "TRIGGER":
                    events[count & EVENT_MASK] = packEvent(state.acc, instr.operands[0]!)
                    count++
                    // acc is deliberately untouched: TRIGGER declares no
                    // writesAcc, so the target must not clobber r0 either.
                    return

                default:
                    throw new Error(`sampstream: unknown op ${instr.ext}`)
            }
        },
        codec: {
            encode(instr: ExtInstr): number[]
            {
                const code = SAMPSTREAM_OPCODES[instr.ext]
                if(code === undefined) throw new Error(`sampstream: cannot encode ${instr.ext}`)
                return instr.ext === "TRIGGER" ? [code, ...encodeLeb128(instr.operands[0]!)] : [code]
            },
            decode(bytes: Uint8Array, offset: number): {instr: ExtInstr; next: number}
            {
                const name = BY_BYTE.get(bytes[offset] ?? -1)
                if(name === undefined) throw new Error(`sampstream: cannot decode byte ${bytes[offset]}`)

                if(name !== "TRIGGER") return {instr: {op: "EXT", ext: name, operands: []}, next: offset + 1}

                const kind = decodeLeb128(bytes, offset + 1)
                return {instr: triggerInstr(kind.value), next: kind.next}
            },
        },
    }
}
