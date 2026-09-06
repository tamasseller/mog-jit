// fuzz — the invalid lane.
//
// Every other lane asks whether a well-formed program computes the same
// answer everywhere. This one asks the opposite question, and it is the one
// the design rests on: an application composing DSL fragments is allowed to
// get it wrong, and something must catch it before a program reaches the
// wire. The JIT itself no longer checks — design.md §12 makes every
// wire-validity check an assert that `-DNDEBUG` strips — so what catches it
// is `lowerProgram`, `signature.ts` or `validateProgram`, and nothing else.
//
// Each breaker violates exactly one named invariant, so a program that gets
// through names which gate was asleep rather than "something is wrong".

import type { Expression, Statement, SwitchCase } from "mog-core"
import type { GenProgram } from "./corpus"
import { calleesOf, procName } from "./corpus"
import { childBodies, eachStatement, declaredNames } from "./walk"
import { Rng } from "./rng"

export interface Breaker
{
    /** The invariant broken, as it would be named in a bug report. */
    name: string
    /** Applied to a copy; returns null where this program has no site. */
    apply: (gen: GenProgram, rng: Rng) => GenProgram | null
}

const clone = (gen: GenProgram): GenProgram =>
    ({procs: gen.procs.map(p => ({args: [...p.args], body: JSON.parse(JSON.stringify(p.body)) as Statement[]}))})

// ── where a breaker may legitimately strike ─────────────────────────────
//
// Two places look like program text and are not part of the program.
// `lowerProgram` walks from the entry, so a procedure nothing calls is never
// lowered at all; and a statement after one that always terminates is
// dropped. Breaking either proves nothing — it produced most of this lane's
// early false escapes.

/** Entry-reachable procedure indices, in order. */
function reachableProcs(gen: GenProgram): number[]
{
    const seen = new Set<number>([0])
    const queue = [0]
    while(queue.length > 0)
    {
        const i = queue.shift()!
        const p = gen.procs[i]
        if(p === undefined) continue
        for(const callee of calleesOf(p.body)) if(!seen.has(callee)) { seen.add(callee); queue.push(callee) }
    }
    return [...seen].sort((a, b) => a - b).filter(i => i < gen.procs.length)
}

const isTrap = (s: Statement): boolean =>
    s.type === "ExpressionStatement" && s.expression.type === "CallExpression"
    && s.expression.callee.name === "trap"

function alwaysTerminates(s: Statement): boolean
{
    if(s.type === "ReturnStatement" || isTrap(s)) return true
    if(s.type === "BlockStatement") return livePrefix(s.body).some(alwaysTerminates)
    if(s.type === "IfStatement")
        return s.alternate !== null
            && alwaysTerminates(s.consequent as Statement) && alwaysTerminates(s.alternate as Statement)
    return false
}

/** The statements of `list` that can run: up to and including the first that
 *  always terminates. */
function livePrefix(list: readonly Statement[]): Statement[]
{
    const out: Statement[] = []
    for(const s of list) { out.push(s); if(alwaysTerminates(s)) break }
    return out
}

/** Every live statement of a body, nested bodies included. */
function liveStatements(body: readonly Statement[], visit: (s: Statement) => void): void
{
    for(const s of livePrefix(body))
    {
        visit(s)
        for(const nested of childBodies(s)) liveStatements(nested, visit)
    }
}

/** `certain` skips the operands an evaluation may never reach: a short-
 *  circuit's right-hand side and a ternary's arms. A breaker whose violation
 *  only shows when the code *runs* has to land somewhere that runs. */
function walkExpr(e: Expression, visit: (e: Expression) => void, certain = false): void
{
    visit(e)
    const anyE = e as unknown as Record<string, unknown>
    const skip = !certain ? []
        : e.type === "LogicalExpression" ? ["right"]
        : e.type === "ConditionalExpression" ? ["consequent", "alternate"] : []

    for(const key of ["left", "right", "argument", "test", "consequent", "alternate", "expression"])
    {
        if(skip.includes(key)) continue
        const v = anyE[key]
        if(v !== null && typeof v === "object" && (v as {type?: string}).type !== undefined)
            walkExpr(v as Expression, visit, certain)
    }
    if(Array.isArray(anyE["arguments"])) for(const a of anyE["arguments"] as Expression[]) walkExpr(a, visit, certain)
}

/** Expressions in code that can actually run. */
function allExpressions(body: Statement[], certain = false): Expression[]
{
    const out: Expression[] = []
    const take = (e: Expression): void => walkExpr(e, x => out.push(x), certain)

    liveStatements(body, s =>
    {
        const anyS = s as unknown as Record<string, unknown>
        for(const key of ["expression", "test", "argument", "discriminant"])
        {
            const v = anyS[key]
            if(v !== null && v !== undefined && typeof v === "object" && (v as {type?: string}).type !== undefined)
                take(v as Expression)
        }
        // A `for`'s update runs after the body completes normally, so a body
        // that always terminates leaves it dead and the lowerer drops it.
        if(s.type === "ForStatement")
        {
            if(!alwaysTerminates(s.body as Statement) && s.update !== null) take(s.update)
            if(s.init !== null && s.init.type === "VariableDeclaration")
                for(const d of s.init.declarations) if(d.init !== null) take(d.init)
        }
        else if(anyS["update"] !== null && anyS["update"] !== undefined
            && typeof anyS["update"] === "object") take(anyS["update"] as Expression)

        if(s.type === "VariableDeclaration")
            for(const d of s.declarations) if(d.init !== null) take(d.init)
    })
    return out
}

/** A procedure the entry can reach — the only kind worth breaking. */
const pickProc = (gen: GenProgram, rng: Rng): number =>
{
    const live = reachableProcs(gen)
    return live[rng.int(live.length)] ?? 0
}

/** Reachable procedures, for the breakers that scan every site. */
const liveProcs = (gen: GenProgram) => reachableProcs(gen).map(i => gen.procs[i]!)

const one = (value: number): Expression => ({type: "Literal", value, raw: String(value)})

// Every site, not one at random. `return x; return y;` is legal and the
// lowerer drops the second outright, so a breaker that picks one site can
// land in code nothing ever lowers and look like an escape it is not.

export const BREAKERS: Breaker[] = [
    {
        // signature.ts deduces a procedure's signature from the returns it
        // finds, and refuses a body that does both. Behind an `if`, so the
        // valued return after it stays reachable — a bare `return;` spliced
        // in front makes everything below it dead and the body consistent.
        name: "mixed return arity",
        apply: (gen, rng) =>
        {
            const g = clone(gen)
            const p = g.procs[pickProc(g, rng)]!
            p.body.unshift({
                type: "IfStatement", test: one(1),
                consequent: {type: "BlockStatement", body: [{type: "ReturnStatement", argument: null}]},
                alternate: null})
            return g
        },
    },
    {
        // isa-core.md §8.2: the call graph is acyclic. lower.ts lowers
        // recursion happily and leaves the rejection to validateProgram.
        name: "recursive call",
        apply: (gen, rng) =>
        {
            const g = clone(gen)
            const i = pickProc(g, rng)
            const p = g.procs[i]!
            p.body.unshift({type: "ExpressionStatement", expression: {
                type: "CallExpression", callee: {type: "Identifier", name: procName(i)},
                arguments: p.args.map(() => one(1)),
            }})
            return g
        },
    },
    {
        name: "unbound identifier",
        apply: (gen, rng) =>
        {
            const g = clone(gen)
            const p = g.procs[pickProc(g, rng)]!
            const names = new Set([...p.args, ...declaredNames(p.body)])
            const idents = allExpressions(p.body).filter(e => e.type === "Identifier" && names.has(e.name))
            const victim = rng.maybePick(idents)
            if(victim === undefined) return null
            ;(victim as {name: string}).name = "zzz_unbound"
            return g
        },
    },
    {
        // ast.ts's BreakStatement is legal only as a switch case's closer.
        name: "break outside a switch",
        apply: (gen, rng) =>
        {
            const g = clone(gen)
            g.procs[pickProc(g, rng)]!.body.unshift({type: "BreakStatement"})
            return g
        },
    },
    {
        name: "call arity mismatch",
        apply: (gen) =>
        {
            const g = clone(gen)
            let any = false
            for(const p of liveProcs(g))
            {
                for(const e of allExpressions(p.body))
                {
                    if(e.type !== "CallExpression" || !/^p[0-9]+$/.test(e.callee.name)) continue
                    const c = e as unknown as {arguments: Expression[]}
                    c.arguments = c.arguments.length > 0 ? c.arguments.slice(0, -1) : [one(1)]
                    any = true
                }
            }
            return any ? g : null
        },
    },
    {
        // The grammar accepts `/` and `%`; rules.ts has no OP_TABLE entry, so
        // neither can lower at all.
        name: "division operator",
        apply: (gen, rng) =>
        {
            const g = clone(gen)
            const op = rng.chance(0.5) ? "/" : "%"
            let any = false
            for(const p of liveProcs(g))
            {
                for(const e of allExpressions(p.body))
                {
                    if(e.type !== "BinaryExpression") continue
                    ;(e as unknown as {operator: string}).operator = op
                    any = true
                }
            }
            return any ? g : null
        },
    },
    {
        // isa-core.md §4.1 as narrowed by campaign finding 5: a shift by 32
        // or more is not defined. Not always a *static* violation —
        // validateProgram range-checks the immediate form, but the same
        // amount reached through a register is a runtime value no validator
        // can see, and the reference VM raises `UnspecifiedShiftAmount`
        // instead. Both count: the program does not get a defined answer.
        name: "shift amount of 32 or more",
        apply: (gen, rng) =>
        {
            const g = clone(gen)
            const amount = one(rng.pick([32, 33, 64, 255, 0xffffffff]))
            let any = false
            for(const p of liveProcs(g))
            {
                for(const e of allExpressions(p.body, /*certain=*/true))
                {
                    if(e.type !== "BinaryExpression" || (e.operator !== "<<" && e.operator !== ">>")) continue
                    ;(e as unknown as {right: Expression}).right = amount
                    any = true
                }
            }
            return any ? g : null
        },
    },
    {
        name: "duplicate case label",
        apply: (gen) =>
        {
            const g = clone(gen)
            for(const p of liveProcs(g))
            {
                let done = false
                eachStatement(p.body, s =>
                {
                    if(done || s.type !== "SwitchStatement") return
                    const labelled = s.cases.filter(c => c.test !== null)
                    if(labelled.length < 2) return
                    ;(labelled[1] as SwitchCase).test = labelled[0]!.test
                    done = true
                })
                if(done) return g
            }
            return null
        },
    },
    {
        // lower.ts's caseCloser: a non-breaking case must be followed by the
        // case one label higher.
        name: "fall-through to a non-adjacent label",
        apply: (gen, rng) =>
        {
            const g = clone(gen)
            for(const p of liveProcs(g))
            {
                let done = false
                eachStatement(p.body, s =>
                {
                    if(done || s.type !== "SwitchStatement") return
                    const labelled = s.cases.filter(c => c.test !== null)
                    if(labelled.length < 2) return
                    const first = labelled[0] as SwitchCase
                    const last = first.consequent[first.consequent.length - 1]
                    if(last === undefined || last.type !== "BreakStatement") return
                    // Dropping the break only opens a fall-through if what is
                    // left does not terminate on its own.
                    const before = first.consequent[first.consequent.length - 2]
                    if(before === undefined || before.type === "ReturnStatement") return
                    // Open the first case, and move the next one's label out of reach.
                    ;(first as {consequent: Statement[]}).consequent = first.consequent.slice(0, -1) as Statement[]
                    ;(labelled[1] as SwitchCase).test = one(900 + rng.int(50))
                    done = true
                })
                if(done) return g
            }
            return null
        },
    },
    {
        // Not merely a default without `break`: one written last needs none,
        // since `caseCloser` closes any clause with nothing after it. The
        // violation is a default that runs on, which names no case to
        // continue into.
        name: "default falls through into a case",
        apply: (gen) =>
        {
            const g = clone(gen)
            for(const p of liveProcs(g))
            {
                let done = false
                eachStatement(p.body, s =>
                {
                    if(done || s.type !== "SwitchStatement") return
                    const at = s.cases.findIndex(c => c.test === null)
                    if(at < 0 || s.cases.length < 2) return
                    const dflt = s.cases[at] as SwitchCase
                    const last = dflt.consequent[dflt.consequent.length - 1]
                    if(last === undefined || last.type !== "BreakStatement") return
                    ;(dflt as {consequent: Statement[]}).consequent = dflt.consequent.slice(0, -1) as Statement[]
                    // Move it off the end, so something is written after it.
                    const cases = s.cases as SwitchCase[]
                    cases.splice(at, 1)
                    cases.splice(Math.max(0, cases.length - 1), 0, dflt)
                    done = true
                })
                if(done) return g
            }
            return null
        },
    },
    {
        // The other way round: a call naming a procedure the graph has not got.
        name: "call to a procedure that does not exist",
        apply: (gen) =>
        {
            const g = clone(gen)
            let any = false
            for(const p of liveProcs(g))
            {
                for(const e of allExpressions(p.body))
                {
                    if(e.type !== "CallExpression" || !/^p[0-9]+$/.test(e.callee.name)) continue
                    ;(e as unknown as {callee: {name: string}}).callee.name = procName(g.procs.length + 3)
                    any = true
                }
            }
            return any ? g : null
        },
    },
]
