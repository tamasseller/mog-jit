// fuzz — the unsequenced-effect filter.
//
// The DSL leaves a binary operator's two operands unsequenced, exactly as C
// does (lift.ts: "Lifting only moves evaluation across operands of one
// expression, which C leaves unsequenced"), and the lowerer really does
// reorder them — builders.ts's `pickBinaryOrder` picks whichever ordering
// scores better on stack depth. So a program whose two operands both have
// effects has no single right answer, and comparing three engines on it
// manufactures mismatches that are not bugs.
//
// Dropping those here is what lets everything downstream demand strict
// equality instead of carrying per-engine exceptions.
//
// A bare "has side effects" flag is not enough: a *read* in one operand
// conflicts with a *write* in the other, and `x += f()` desugars to
// `x = x + f()`, which is exactly that shape. So the lattice tracks which
// names are read and written, whether the extension buffer is touched, and
// whether the subtree can trap.
//
// This is the static half. Shift amounts of 32 or more are undefined too
// (isa-core.md §4.1) but only known at run time, so the evaluator raises
// them dynamically — two filters, not one.

import type { Expression, Statement } from "mog-core"
import { expr as printExpr } from "./print"
import { calleesOf } from "./corpus"
import type { GenProgram } from "./corpus"
import { childExpressions, eachStatement } from "./walk"

type Mem = "none" | "read" | "write"

export interface Effects
{
    reads: Set<string>
    writes: Set<string>
    mem: Mem
    /** Can abandon the rest of the expression — `trap(...)`, directly or
     *  through a call. Divergence is deliberately not modelled: a program
     *  that does not terminate is dropped on the step limit long before any
     *  comparison, so it never reaches the question this asks. */
    terminates: boolean
}

const none = (): Effects => ({reads: new Set(), writes: new Set(), mem: "none", terminates: false})

const MEM_READ = new Set(["ld8", "ld16", "ld32", "memcmp", "slicecmp"])
const MEM_WRITE = new Set(["st8", "st16", "st32", "memmove"])
const PURE = new Set(["clz", "revbits"])

const strongerMem = (a: Mem, b: Mem): Mem =>
    a === "write" || b === "write" ? "write" : a === "read" || b === "read" ? "read" : "none"

function union(parts: readonly Effects[]): Effects
{
    const out = none()
    for(const p of parts)
    {
        for(const r of p.reads) out.reads.add(r)
        for(const w of p.writes) out.writes.add(w)
        out.mem = strongerMem(out.mem, p.mem)
        out.terminates ||= p.terminates
    }
    return out
}

const touches = (e: Effects): boolean => e.writes.size > 0 || e.mem !== "none" || e.terminates

const intersects = (a: ReadonlySet<string>, b: ReadonlySet<string>): boolean =>
{
    for(const x of a) if(b.has(x)) return true
    return false
}

/** Does the order of `a` and `b` change what the program computes? */
function conflict(a: Effects, b: Effects): boolean
{
    if(intersects(a.writes, b.reads) || intersects(b.writes, a.reads) || intersects(a.writes, b.writes)) return true
    if(a.mem === "write" && b.mem !== "none") return true
    if(b.mem === "write" && a.mem !== "none") return true
    // A trap abandons the expression, so whether the other operand's effects
    // happened at all depends on which ran first — and two traps disagree on
    // which code comes out.
    if(a.terminates && touches(b)) return true
    if(b.terminates && touches(a)) return true
    return false
}

export class Unsequenced extends Error
{
    constructor(readonly site: string)
    {
        super(`unsequenced operands with conflicting effects: ${site}`)
        this.name = "Unsequenced"
    }
}

/** Per-procedure summary — only what escapes a call. Locals never do: an
 *  argument is passed by value and the language has no pointers, so the
 *  extension buffer is the one channel between procedures. */
type Summaries = Map<number, {mem: Mem; terminates: boolean}>

function analyzeExpr(e: Expression, summaries: Summaries): Effects
{
    switch(e.type)
    {
        case "Literal": return none()
        case "Identifier": { const out = none(); out.reads.add(e.name); return out }

        case "CastExpression": return analyzeExpr(e.argument, summaries)
        case "UnaryExpression": return analyzeExpr(e.argument, summaries)

        case "UpdateExpression":
        {
            const out = analyzeExpr(e.argument, summaries)
            if(e.argument.type === "Identifier") out.writes.add(e.argument.name)
            return out
        }

        case "AssignmentExpression":
        {
            // `left` is an Identifier, so there is nothing to evaluate on
            // that side and nothing to order against; the write itself is
            // sequenced after the value, as in C.
            const out = analyzeExpr(e.right, summaries)
            if(e.operator !== "=") out.reads.add(e.left.name)
            out.writes.add(e.left.name)
            return out
        }

        case "LogicalExpression":
        {
            // `&&`/`||` sequence their operands, and so a conflict between
            // them is ordinary, defined behaviour.
            return union([analyzeExpr(e.left, summaries), analyzeExpr(e.right, summaries)])
        }

        case "ConditionalExpression":
        {
            // The test runs first and the two arms are mutually exclusive,
            // so no pair of these three is ever racing.
            return union([analyzeExpr(e.test, summaries), analyzeExpr(e.consequent, summaries),
                          analyzeExpr(e.alternate, summaries)])
        }

        case "BinaryExpression":
        {
            const left = analyzeExpr(e.left, summaries)
            const right = analyzeExpr(e.right, summaries)
            if(conflict(left, right)) throw new Unsequenced(printExpr(e))
            return union([left, right])
        }

        case "CallExpression":
        {
            const args = e.arguments.map(a => analyzeExpr(a, summaries))

            // Arguments are unsequenced, the same as a binary operator's
            // operands. The lowerer happens to push them in source order
            // today, but nothing in the language says it must.
            for(let i = 0; i < args.length; i++)
            {
                for(let k = i + 1; k < args.length; k++)
                {
                    if(conflict(args[i]!, args[k]!)) throw new Unsequenced(printExpr(e))
                }
            }

            const out = union(args)
            const name = e.callee.name

            if(name === "trap") { out.terminates = true; return out }
            if(PURE.has(name)) return out
            if(MEM_READ.has(name)) { out.mem = strongerMem(out.mem, "read"); return out }
            if(MEM_WRITE.has(name)) { out.mem = "write"; return out }

            const match = /^p([0-9]+)$/.exec(name)
            if(match !== null)
            {
                const summary = summaries.get(Number(match[1]))
                if(summary !== undefined)
                {
                    out.mem = strongerMem(out.mem, summary.mem)
                    out.terminates ||= summary.terminates
                }
                else
                {
                    // Not yet summarised: assume the worst rather than
                    // quietly letting a conflict through.
                    out.mem = "write"
                    out.terminates = true
                }
            }
            return out
        }
    }
}

/** Every expression in `body`, each analysed on its own — statements are
 *  sequence points, so nothing is compared across one. */
function analyzeBody(body: readonly Statement[], summaries: Summaries): Effects
{
    const parts: Effects[] = []
    eachStatement(body, s => { for(const e of childExpressions(s)) parts.push(analyzeExpr(e, summaries)) })
    return union(parts)
}

/** What a call to each procedure can do to state its caller can observe.
 *  Built from the highest index down, which is a valid order because a call
 *  only ever targets a higher one (corpus.ts's DAG invariant). */
function summarize(gen: GenProgram): Summaries
{
    const summaries: Summaries = new Map()
    for(let i = gen.procs.length - 1; i >= 0; i--)
    {
        const body = gen.procs[i]!.body
        let mem: Mem = "none"
        let terminates = false

        // Its own expressions, plus whatever it calls.
        try
        {
            const own = analyzeBody(body, summaries)
            mem = own.mem
            terminates = own.terminates
        }
        catch(e)
        {
            // A conflict inside this procedure is reported by `analyze`
            // below; for the summary, all that matters is that it may do
            // anything its body could.
            if(!(e instanceof Unsequenced)) throw e
            mem = "write"
            terminates = true
        }

        for(const callee of calleesOf(body))
        {
            const summary = summaries.get(callee)
            if(summary === undefined) continue
            mem = strongerMem(mem, summary.mem)
            terminates ||= summary.terminates
        }

        summaries.set(i, {mem, terminates})
    }
    return summaries
}

export interface UbReport
{
    /** True when the program's result depends on an evaluation order the
     *  language does not fix — drop it rather than compare engines on it. */
    unsequenced: boolean
    site?: string
}

export function analyze(gen: GenProgram): UbReport
{
    const summaries = summarize(gen)
    try
    {
        for(const p of gen.procs) analyzeBody(p.body, summaries)
        return {unsequenced: false}
    }
    catch(e)
    {
        if(e instanceof Unsequenced) return {unsequenced: true, site: e.site}
        throw e
    }
}
