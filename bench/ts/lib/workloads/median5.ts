// Workload 3: a five-tap median filter.
//
// The nonlinear, compare-heavy case, and the one that most needs the DSL's
// signed types: a median over samples that go below zero is meaningless
// under an unsigned compare, and `v < 0` was silently false before those
// landed. Every comparison here is between `i32` locals, so each lowers to
// LT_S/GT_S rather than the unsigned form.
//
// A sorting network rather than a median-specific shortcut. A network is
// branch-per-comparator with no data-dependent control flow to speak of,
// which is what makes it a fair thing to compile two ways — and Cortex-M0
// has no IT blocks, so GCC has to branch for each compare-exchange too.
// Neither side gets a conditional-move it can hide the work in.
//
// The look-back is `sample_at(i - k)`, which is why the extension has no
// separate peek op: a masked ring plus an index the program owns gives the
// window for free.

import {proc, ir, type Procedure} from "mog-core"
import {SAMP_IN_SAMPLES, IN_MASK} from "../sampstream_ext"
import type {Sink, Workload} from "./workload"

const TAPS = 5

/* Knuth's nine-comparator network for five elements (TAOCP vol. 3, §5.3.4).
 * Nine is optimal for a full sort of five; a median-only network needs six,
 * but the sorted output is worth having here — it is what lets the
 * reference check the whole ordering rather than only the middle element,
 * and the extra three comparators are the same shape as the other six. */
const NETWORK: readonly (readonly [number, number])[] = [
    [0, 1], [3, 4], [2, 4], [2, 3], [1, 4], [0, 3], [0, 2], [1, 3], [1, 2],
]

const MEDIAN_INDEX = 2

/** Brute force over every ordering of five distinct values. Nine
 *  comparators in the wrong order sorts almost everything, so an eyeballed
 *  network is not evidence. */
export function networkSorts(): boolean
{
    const permute = (xs: number[]): number[][] =>
        xs.length <= 1 ? [xs]
            : xs.flatMap((x, i) =>
                permute([...xs.slice(0, i), ...xs.slice(i + 1)]).map(rest => [x, ...rest]))

    for(const perm of permute([0, 1, 2, 3, 4]))
    {
        const v = [...perm]
        for(const [a, b] of NETWORK) if(v[a]! > v[b]!) [v[a], v[b]] = [v[b]!, v[a]!]
        for(let k = 0; k < TAPS; k++) if(v[k] !== k) return false
    }

    return true
}

const SLOTS = ["a", "b", "c", "d", "e"] as const

function body(): Procedure
{
    /* One shared temporary rather than a block-scoped one per comparator:
     * the DSL has no bare block, so a scope only exists as an `if` body,
     * and declaring inside each would make the register allocator's job
     * depend on nesting for no gain. */
    const swaps = NETWORK.map(([x, y]) =>
        `    if (${SLOTS[x]} > ${SLOTS[y]}) { t = ${SLOTS[x]}; ${SLOTS[x]} = ${SLOTS[y]}; `
        + `${SLOTS[y]} = t; }`).join("\n")

    const loads = SLOTS.map((s, k) =>
        `    ${s} = sample_at(i - ${TAPS - 1 - k});`).join("\n")

    return proc(["n"], ir`
i32 a = 0;
i32 b = 0;
i32 c = 0;
i32 d = 0;
i32 e = 0;
i32 t = 0;
for (u32 i = 0; i != n; i++)
{
${loads}
${swaps}
    out_at(i, ${SLOTS[MEDIAN_INDEX]});
}
return n;
`)
}

/** A smooth baseline with periodic impulse spikes — the thing a median
 *  filter exists to remove — crossing zero so the signed comparisons are
 *  doing real work. */
function samples(): Int16Array
{
    const s = new Int16Array(SAMP_IN_SAMPLES)

    for(let i = 0; i < s.length; i++)
    {
        const base = 1400 * Math.sin(i / 23) + 500 * Math.sin(i / 5.5)
        const spike = i % 17 === 0 ? (i % 34 === 0 ? 9000 : -9000) : 0

        s[i] = Math.round(base + spike)
    }

    return s
}

function reference(input: Int16Array, n: number, sink: Sink): number
{
    for(let i = 0; i < n; i++)
    {
        const v: number[] = []
        for(let k = 0; k < TAPS; k++) v.push(input[(i - (TAPS - 1 - k)) & IN_MASK]!)

        for(const [x, y] of NETWORK) if(v[x]! > v[y]!) [v[x], v[y]] = [v[y]!, v[x]!]

        sink.out(i, v[MEDIAN_INDEX]!)
    }

    return n
}

export const median5: Workload = {
    name: "median5",
    kernel: "refMedian5",
    params: {MEDIAN_TAPS: TAPS},
    proc: body,
    samples,
    reference,
    step: 1,
}
