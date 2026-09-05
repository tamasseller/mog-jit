// fuzz — the printer's own property: parse(print(ast)) is ast.
//
// Asserted on every candidate, not just in a test. A failure is a parser or
// printer bug and is itself a finding — which is the second reason the
// generator goes through source text rather than building `IrFragment`
// bodies directly.

import { ir } from "mog-core"
import type { Statement } from "mog-core"
import { print } from "./print"

/** `raw` is the parser's own source slice and `signed` is stamped later by
 *  types.ts; neither is part of what the tree means, so neither takes part
 *  in the comparison. */
export function normalize(node: unknown): unknown
{
    if(Array.isArray(node)) return node.map(normalize)
    if(node === null || typeof node !== "object") return node

    const out: Record<string, unknown> = {}
    for(const key of Object.keys(node as object).sort())
    {
        if(key === "raw" || key === "signed") continue
        out[key] = normalize((node as Record<string, unknown>)[key])
    }
    return out
}

export class RoundTripError extends Error
{
    constructor(readonly source: string, readonly detail: string)
    {
        super(`round trip: ${detail}\n--- printed ---\n${source}`)
        this.name = "RoundTripError"
    }
}

export interface RoundTrip
{
    source: string
    /** What the real parser made of it — the tree that goes on to the
     *  lowerer, so nothing downstream works from one this module built. */
    body: readonly Statement[]
}

/** Print, reparse, and compare. The reparsed tree is returned rather than
 *  discarded: it is exactly what the caller needs next, and parsing is the
 *  most expensive thing in the inner loop to do twice. */
export function roundTrip(body: readonly Statement[]): RoundTrip
{
    const source = print(body)

    let reparsed: readonly Statement[]
    try { reparsed = ir`${source}`.body }
    catch(e) { throw new RoundTripError(source, `does not reparse — ${(e as Error).message.split("\n")[0]}`) }

    const before = JSON.stringify(normalize(body))
    const after = JSON.stringify(normalize(reparsed))
    if(before !== after) throw new RoundTripError(source, `reparsed to a different tree\n  in:  ${before}\n  out: ${after}`)

    return {source, body: reparsed}
}
