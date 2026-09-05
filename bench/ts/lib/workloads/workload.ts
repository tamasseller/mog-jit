// What a benchmark workload has to provide.
//
// Three things that must not be allowed to drift apart: the DSL body the
// JIT compiles, the C transcription the reference kernel compiles, and a
// TypeScript transcription that decides what the right answer is. The
// parameters are declared once here and generated into a header the C side
// includes, so at least the constants cannot be tuned in one place and not
// the others; the bodies themselves are kept honest by check-workload.ts
// and by the image comparing all three at run time.

import type {Procedure} from "mog-core"

/** Where a reference run's side effects go — the same two sinks the
 *  extension gives a DSL program. */
export interface Sink
{
    trigger(index: number, kind: number): void
    out(index: number, value: number): void
}

export interface Workload
{
    name: string

    /** The C reference kernel's symbol, for size measurement and for the
     *  generated `REF_KERNEL` dispatch. */
    kernel: string

    /** Emitted as `#define`s the C kernels compile against. */
    params: Readonly<Record<string, number>>

    proc(): Procedure
    samples(): Int16Array

    /** The answer, independent of both compiled sides. Returns what the
     *  procedure returns. */
    reference(input: Int16Array, n: number, sink: Sink): number

    /** Sample counts must be a multiple of this — the IQ detector steps
     *  four samples at a time, so a count that is not a multiple of four
     *  would run past its loop bound. */
    step: number
}
