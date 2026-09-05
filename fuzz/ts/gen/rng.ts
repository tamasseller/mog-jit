// fuzz — the generator's randomness.
//
// Seeded and deterministic, because a finding's repro is `(corpus entry,
// seed, generator version)` and nothing else: no byte tape, no saved input
// file. Mulberry32 — small, fast, and good enough for choosing between a
// few dozen alternatives.

export class Rng
{
    private state: number

    constructor(seed: number) { this.state = seed >>> 0 }

    /** [0, 1) */
    next(): number
    {
        this.state = (this.state + 0x6d2b79f5) >>> 0
        let t = this.state
        t = Math.imul(t ^ (t >>> 15), t | 1)
        t ^= t + Math.imul(t ^ (t >>> 7), t | 61)
        return ((t ^ (t >>> 14)) >>> 0) / 4294967296
    }

    /** [0, n) */
    int(n: number): number { return Math.floor(this.next() * n) }

    /** [lo, hi] */
    between(lo: number, hi: number): number { return lo + this.int(hi - lo + 1) }

    chance(p: number): boolean { return this.next() < p }

    pick<T>(items: readonly T[]): T
    {
        if(items.length === 0) throw new Error("rng.pick: empty")
        return items[this.int(items.length)]!
    }

    /** `pick`, or undefined for an empty list — the common shape here, where
     *  "no variable is in scope yet" is ordinary rather than exceptional. */
    maybePick<T>(items: readonly T[]): T | undefined
    {
        return items.length === 0 ? undefined : items[this.int(items.length)]
    }

    shuffled<T>(items: readonly T[]): T[]
    {
        const out = [...items]
        for(let i = out.length - 1; i > 0; i--)
        {
            const j = this.int(i + 1)
            const a = out[i]!, b = out[j]!
            out[i] = b; out[j] = a
        }
        return out
    }
}
