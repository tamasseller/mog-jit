// fuzz — the scoped rewrite walk.
//
// Mutation happens *under* the language's invariants rather than being
// repaired afterwards: the walk carries what is legal at each site — which
// variables are visible, which procedures may be called, whether a `break`
// would still be its case's closer — and only ever picks from mutations
// that site accepts. Everything it emits is therefore well-scoped and
// well-typed by construction, which is what makes validator approval the
// normal outcome rather than a lottery.
//
// Four invariants are load-bearing and each has a real trap behind it:
//
//   - A call only ever targets a higher procedure index. isa-core.md §8.2
//     forbids recursion, and lower.ts lowers it happily and leaves the
//     rejection to the validator.
//   - No `return;` is ever emitted, and every body is closed with a valued
//     one. signature.ts throws on a body that does both, and a void entry's
//     result is unspecified (§8.7) — neither is worth generating.
//   - `break` stays its case's last statement (ast.ts's `BreakStatement`).
//   - `/` and `%` are never emitted: the grammar accepts them but rules.ts
//     has no `OP_TABLE` entry, so they do not lower at all.

import type {
    BinaryOperator, Expression, Identifier, PrimType, Statement, SwitchCase, VariableDeclarator,
} from "mog-core"
import { mapOver } from "mog-core"
import { Rng } from "./rng"
import { calleesOf, procName } from "./corpus"
import type { GenProgram } from "./corpus"
import { childBodies, declaredNames, eachExpression, heightOf } from "./walk"
import { analyze } from "./ub"

// ── the alphabet ────────────────────────────────────────────────────────

const ARITH: BinaryOperator[] = ["+", "-", "*"]
const BITWISE: BinaryOperator[] = ["|", "^", "&"]
const SHIFT: BinaryOperator[] = ["<<", ">>"]
const COMPARE: BinaryOperator[] = ["==", "!=", "<", "<=", ">", ">="]
const BINARY_CLASSES: BinaryOperator[][] = [ARITH, BITWISE, SHIFT, COMPARE]

const NARROW: PrimType[] = ["u16", "u8", "i16", "i8"]
const ALL_TYPES: PrimType[] = ["u32", "u16", "u8", "i32", "i16", "i8"]

/** How tall a generated expression may get. Mutation wraps far more often
 *  than it unwraps, so across generations a corpus that fed itself back
 *  would nest without limit — and lowering a tree of depth 20 costs orders
 *  of magnitude more than one of depth 8, which is where a campaign's whole
 *  budget goes. Above this only the rewrites that do not grow the tree are
 *  offered. */
const MAX_EXPR_HEIGHT = 8

/** Values that sit on an edge of something: a word boundary, a shift's
 *  defined range (isa-core.md §4.1 stops at 31), a byte or halfword mask. */
const BOUNDARY = [0, 1, 2, 3, 7, 8, 15, 16, 31, 32, 255, 256, 65535, 65536, 0x7fffffff, 0x80000000, 0xffffffff]

/** Calls that are never rewritten and never grafted into a value position:
 *  `trap` terminates and takes only a constant, `memmove` declares
 *  `killsAcc` so nothing downstream may read its result. */
const OPAQUE_CALLS = new Set(["trap", "memmove"])

const UNARY_BUILTINS = ["clz", "revbits"]
const LOAD_BUILTINS = ["ld8", "ld16", "ld32"]
const STORE_BUILTINS = ["st8", "st16", "st32"]

// ── environment ─────────────────────────────────────────────────────────

interface Var { name: string; type: PrimType }

interface Scope
{
    vars: Var[]
    /** Set only directly inside a `switch` case body, where a trailing
     *  `break` is legal and anywhere else is not. */
    inCase: boolean
    /** Inside a `while`/`do` body, where dropping a statement is how the
     *  loop's own countdown stops counting down. */
    inLoop: boolean
}

interface Ctx
{
    rng: Rng
    fresh: () => string
    /** `p<j>` for every j above the procedure being mutated — the DAG. */
    callable: {name: string; arity: number}[]
    /** Expressions harvested from elsewhere, for splicing. Every identifier
     *  in one is rebound to a visible variable before it is used. */
    donors: Expression[]
    /** Probability that any one site is mutated. */
    rate: number
    applied: number
}

const childScope = (s: Scope, inCase = false, inLoop = s.inLoop): Scope =>
    ({vars: [...s.vars], inCase, inLoop})

// ── expression construction ─────────────────────────────────────────────

const ident = (name: string): Identifier => ({type: "Identifier", name})
const lit = (value: number): Expression => ({type: "Literal", value: value >>> 0, raw: String(value >>> 0)})

/** A shift by 32 or more is not defined (isa-core.md §4.1, narrowed by
 *  fuzzing-campaign.md finding 5) and the validator rejects a constant one
 *  outright, so a generated amount stays inside the range rather than
 *  spending the mutant on a known rejection. */
function shiftSafe(operator: BinaryOperator, right: Expression, ctx: Ctx): Expression
{
    if(operator !== "<<" && operator !== ">>") return right
    if(right.type === "Literal") return right.value <= 31 ? right : lit(right.value & 31)

    // Idempotent: this runs on every pass, so re-wrapping an already-masked
    // amount would add `& 31` once per generation and nest the tree without
    // bound, which costs the lowerer dearly and means nothing.
    if(right.type === "BinaryExpression" && right.operator === "&"
        && right.right.type === "Literal" && right.right.value === 31) return right

    return {type: "BinaryExpression", operator: "&", left: right, right: lit(31)}
}

function leaf(ctx: Ctx, scope: Scope): Expression
{
    const v = ctx.rng.chance(0.55) ? ctx.rng.maybePick(scope.vars) : undefined
    return v === undefined ? lit(ctx.rng.pick(BOUNDARY)) : ident(v.name)
}

/** A small expression, used wherever a mutation needs a fresh operand. */
function build(ctx: Ctx, scope: Scope, depth: number): Expression
{
    if(depth <= 0 || ctx.rng.chance(0.4)) return leaf(ctx, scope)

    switch(ctx.rng.int(6))
    {
        case 0:
        {
            const operator = ctx.rng.pick(ctx.rng.pick(BINARY_CLASSES))
            return {type: "BinaryExpression", operator,
                    left: build(ctx, scope, depth - 1), right: shiftSafe(operator, build(ctx, scope, depth - 1), ctx)}
        }
        case 1: return {type: "UnaryExpression", operator: ctx.rng.pick(["-", "~", "!"] as const),
                        argument: build(ctx, scope, depth - 1), prefix: true}
        case 2: return {type: "CastExpression", varType: ctx.rng.pick(NARROW), argument: build(ctx, scope, depth - 1)}
        case 3: return {type: "ConditionalExpression", test: build(ctx, scope, depth - 1),
                        consequent: build(ctx, scope, depth - 1), alternate: build(ctx, scope, depth - 1)}
        case 4: return {type: "CallExpression", callee: ident(ctx.rng.pick(UNARY_BUILTINS)),
                        arguments: [build(ctx, scope, depth - 1)]}
        default: return {type: "CallExpression", callee: ident(ctx.rng.pick(LOAD_BUILTINS)),
                         arguments: [build(ctx, scope, depth - 1)]}
    }
}

/** A donor subtree with every identifier rebound into `scope`, so grafting
 *  one can never introduce a name this position cannot see. */
function rebind(e: Expression, ctx: Ctx, scope: Scope): Expression
{
    if(e.type === "Identifier")
    {
        const v = ctx.rng.maybePick(scope.vars)
        return v === undefined ? lit(ctx.rng.pick(BOUNDARY)) : ident(v.name)
    }
    if(e.type === "AssignmentExpression")
    {
        const v = ctx.rng.maybePick(scope.vars)
        // Nothing to assign into: keep the value, drop the assignment.
        if(v === undefined) return rebind(e.right, ctx, scope)
        return {...e, left: ident(v.name), right: rebind(e.right, ctx, scope)}
    }
    if(e.type === "UpdateExpression")
    {
        const v = ctx.rng.maybePick(scope.vars)
        return v === undefined ? lit(1) : {...e, argument: ident(v.name)}
    }
    if(e.type === "CallExpression")
    {
        // A call to a procedure the donor could reach may be unreachable
        // here (the DAG), so it is re-pointed or replaced outright.
        const target = ctx.callable.find(c => c.name === e.callee.name && c.arity === e.arguments.length)
        if(/^p[0-9]+$/.test(e.callee.name) && target === undefined)
            return build(ctx, scope, 1)
    }
    return mapOver(e, child => rebind(child, ctx, scope))
}

// ── expression mutation ─────────────────────────────────────────────────

function mutateExpr(e: Expression, ctx: Ctx, scope: Scope, depth = 0): Expression
{
    // `trap` is a terminator whose argument must be a compile-time constant
    // (rules.ts's `builtin:trap`), and `memmove` destroys acc, so neither
    // survives being wrapped in something that reads its value. Both are
    // left exactly as written.
    if(e.type === "CallExpression" && OPAQUE_CALLS.has(e.callee.name)) return e

    // Recurse first, so a mutation applied here wraps already-mutated
    // children rather than being immediately re-entered. An update's operand
    // is not descended into: it must stay a bare variable.
    const recursed = e.type === "UpdateExpression" ? e : mapOver(e, child => mutateExpr(child, ctx, scope))

    // A shift amount mutated below this node has not passed the range guard
    // that constructing one here would have, so it is masked on the way out.
    const inner = recursed.type === "BinaryExpression"
        ? {...recursed, right: shiftSafe(recursed.operator, recursed.right, ctx)}
        : recursed

    if(!ctx.rng.chance(ctx.rate)) return inner
    ctx.applied++

    // Past the cap only the rewrites that keep the tree the same size or
    // shrink it are on offer. Both terms matter: a short subtree grafted
    // deep still makes the whole tree taller.
    const mayGrow = depth + heightOf(inner) < MAX_EXPR_HEIGHT
    const options: (() => Expression)[] = []

    if(inner.type === "BinaryExpression")
    {
        const cls = BINARY_CLASSES.find(c => c.includes(inner.operator)) ?? ARITH
        const retyped = (operator: BinaryOperator): Expression =>
            ({...inner, operator, right: shiftSafe(operator, inner.right, ctx)})
        options.push(() => retyped(ctx.rng.pick(cls)))
        options.push(() => retyped(ctx.rng.pick(ctx.rng.pick(BINARY_CLASSES))))
        // Swapping a shift's operands moves whatever was on the left into
        // the amount position, so the range guard applies here too.
        options.push(() => ({...inner, left: inner.right, right: shiftSafe(inner.operator, inner.left, ctx)}))
        options.push(() => inner.left)
        options.push(() => inner.right)
    }

    if(inner.type === "LogicalExpression")
    {
        options.push(() => ({...inner, operator: inner.operator === "&&" ? "||" : "&&"}))
        options.push(() => ({...inner, left: inner.right, right: inner.left}))
    }

    if(inner.type === "Literal")
    {
        options.push(() => lit(ctx.rng.pick(BOUNDARY)))
        options.push(() => lit((inner.value + ctx.rng.between(-2, 2)) >>> 0))
    }

    if(inner.type === "Identifier" || inner.type === "Literal") options.push(() => leaf(ctx, scope))

    if(inner.type === "UnaryExpression") options.push(() => ({...inner, operator: ctx.rng.pick(["-", "~", "!"] as const)}))
    if(inner.type === "CastExpression") options.push(() => ({...inner, varType: ctx.rng.pick(NARROW)}))

    if(!mayGrow) return ctx.rng.pick(options.length > 0 ? options : [() => inner])()

    // Shape-independent rewrites, available at every site.
    options.push(() =>
    {
        const operator = ctx.rng.pick(ctx.rng.pick(BINARY_CLASSES))
        return {type: "BinaryExpression", operator, left: inner, right: shiftSafe(operator, build(ctx, scope, 1), ctx)}
    })
    options.push(() => ({type: "UnaryExpression", operator: ctx.rng.pick(["-", "~", "!"] as const), argument: inner, prefix: true}))
    options.push(() => ({type: "CastExpression", varType: ctx.rng.pick(NARROW), argument: inner}))
    options.push(() => ({type: "ConditionalExpression", test: inner,
                         consequent: build(ctx, scope, 1), alternate: build(ctx, scope, 1)}))
    options.push(() => ({type: "LogicalExpression", operator: ctx.rng.pick(["&&", "||"] as const),
                         left: inner, right: build(ctx, scope, 1)}))
    options.push(() => ({type: "CallExpression", callee: ident(ctx.rng.pick(UNARY_BUILTINS)), arguments: [inner]}))
    options.push(() => ({type: "CallExpression", callee: ident(ctx.rng.pick(LOAD_BUILTINS)), arguments: [inner]}))
    options.push(() => build(ctx, scope, 2))

    const callee = ctx.rng.maybePick(ctx.callable)
    if(callee !== undefined)
    {
        options.push(() => ({type: "CallExpression", callee: ident(callee.name),
                             arguments: Array.from({length: callee.arity}, () => build(ctx, scope, 1))}))
    }

    const donor = ctx.rng.maybePick(ctx.donors)
    if(donor !== undefined && depth + heightOf(donor) < MAX_EXPR_HEIGHT)
        options.push(() => rebind(donor, ctx, scope))

    const assignTo = ctx.rng.maybePick(scope.vars)
    if(assignTo !== undefined)
    {
        options.push(() => ({type: "AssignmentExpression", operator: "=", left: ident(assignTo.name), right: inner}))
        options.push(() => ({type: "UpdateExpression", operator: ctx.rng.pick(["++", "--"] as const),
                             argument: ident(assignTo.name), prefix: ctx.rng.chance(0.5)}))
    }

    return ctx.rng.pick(options)()
}

// ── statement construction ──────────────────────────────────────────────

function declare(ctx: Ctx, scope: Scope): Statement
{
    const d: VariableDeclarator = {
        type: "VariableDeclarator",
        varType: ctx.rng.pick(ALL_TYPES),
        id: ident(ctx.fresh()),
        init: build(ctx, scope, 2),
    }
    scope.vars.push({name: d.id.name, type: d.varType})
    return {type: "VariableDeclaration", declarations: [d]}
}

/** `if (c) { }` — both arms of the dispatch empty, which is the one shape
 *  translateIfThen answers by emitting nothing but the test's own effects.
 *  Mutation otherwise never empties a body, so nothing else reaches it. */
function emptyIf(ctx: Ctx, scope: Scope): Statement
{
    return {type: "IfStatement", test: build(ctx, scope, 2),
            consequent: {type: "BlockStatement", body: []},
            alternate: ctx.rng.chance(0.5) ? {type: "BlockStatement", body: []} : null}
}

function sideEffect(ctx: Ctx, scope: Scope): Statement
{
    const v = ctx.rng.maybePick(scope.vars)
    if(v !== undefined && ctx.rng.chance(0.6))
    {
        return {type: "ExpressionStatement", expression:
            {type: "AssignmentExpression", operator: "=", left: ident(v.name), right: build(ctx, scope, 2)}}
    }
    // A store: the extension's own statement shape, and the only way the
    // EXT seam is reached from generated source at all.
    return {type: "ExpressionStatement", expression: {
        type: "CallExpression", callee: ident(ctx.rng.pick(STORE_BUILTINS)),
        arguments: [build(ctx, scope, 1), build(ctx, scope, 1)],
    }}
}

/** A counted loop: fresh induction variable, constant bound, unit step —
 *  the one loop shape that cannot fail to terminate. Mutation never touches
 *  its control, so a generated loop stays bounded however deeply it is
 *  rewritten around. */
function countedLoop(body: Statement[], ctx: Ctx, scope: Scope): Statement
{
    const name = ctx.fresh()
    return {
        type: "ForStatement",
        init: {type: "VariableDeclaration", declarations: [{
            type: "VariableDeclarator", varType: "u32", id: ident(name), init: lit(0)}]},
        test: {type: "BinaryExpression", operator: "<", left: ident(name), right: lit(ctx.rng.between(1, 4))},
        update: {type: "AssignmentExpression", operator: "=", left: ident(name),
                 right: {type: "BinaryExpression", operator: "+", left: ident(name), right: lit(1)}},
        body: {type: "BlockStatement", body},
    }
}

/** The same bound, in the shape whose only branch is the back edge: a
 *  `for`'s exit test branches forward over the whole body, so a body too
 *  long for one is too long for the other and the forward branch is the one
 *  that fails first. */
function countedDoWhile(body: Statement[], ctx: Ctx, scope: Scope): Statement
{
    const name = ctx.fresh()
    return {type: "BlockStatement", body: [
        {type: "VariableDeclaration", declarations: [{
            type: "VariableDeclarator", varType: "u32", id: ident(name), init: lit(0)}]},
        {type: "DoWhileStatement",
         body: {type: "BlockStatement", body: [...body, {type: "ExpressionStatement", expression:
            {type: "AssignmentExpression", operator: "=", left: ident(name),
             right: {type: "BinaryExpression", operator: "+", left: ident(name), right: lit(1)}}}]},
         test: {type: "BinaryExpression", operator: "<", left: ident(name), right: lit(ctx.rng.between(1, 4))}},
    ]}
}

function switchOver(body: Statement[], ctx: Ctx, scope: Scope): Statement
{
    const count = ctx.rng.between(1, 3)
    const cases: SwitchCase[] = []
    for(let i = 0; i < count; i++)
    {
        cases.push({type: "SwitchCase", test: lit(i),
                    consequent: i === 0 ? [...body, {type: "BreakStatement"}] : [sideEffect(ctx, childScope(scope, true)), {type: "BreakStatement"}]})
    }
    if(ctx.rng.chance(0.7)) cases.push({type: "SwitchCase", test: null, consequent: [{type: "BreakStatement"}]})

    return {type: "SwitchStatement",
            discriminant: {type: "BinaryExpression", operator: "&", left: build(ctx, scope, 1), right: lit(3)},
            cases}
}

// ── statement mutation ──────────────────────────────────────────────────

/** Wrap `s` in a new enclosing construct. Never applied to a `break`, whose
 *  legality depends on standing last in its own case. */
function wrap(s: Statement, ctx: Ctx, scope: Scope): Statement
{
    const inner = [s]
    switch(ctx.rng.int(5))
    {
        case 0: return {type: "IfStatement", test: build(ctx, scope, 2),
                        consequent: {type: "BlockStatement", body: inner}, alternate: null}
        case 1: return {type: "IfStatement", test: build(ctx, scope, 2),
                        consequent: {type: "BlockStatement", body: inner},
                        alternate: {type: "BlockStatement", body: [sideEffect(ctx, childScope(scope))]}}
        case 2: return countedLoop(inner, ctx, scope)
        case 3: return {type: "BlockStatement", body: inner}
        default: return switchOver(inner, ctx, scope)
    }
}

function mutateStatements(list: readonly Statement[], ctx: Ctx, scope: Scope): Statement[]
{
    // A trailing `break` is its case's closer and stays exactly there.
    const closer = scope.inCase && list.length > 0 && list[list.length - 1]!.type === "BreakStatement"
        ? list[list.length - 1]!
        : undefined
    const source = closer === undefined ? list : list.slice(0, -1)

    const out: Statement[] = []

    for(const s of source)
    {
        // Insert before.
        if(ctx.rng.chance(ctx.rate * 0.5))
        {
            ctx.applied++
            if(ctx.rng.chance(0.08)) out.push(emptyIf(ctx, scope))
            else out.push(ctx.rng.chance(0.5) ? declare(ctx, scope) : sideEffect(ctx, scope))
        }

        // A declaration stays exactly where it stands. Dropping one orphans
        // every later use of the name, wrapping one buries it in a scope
        // that ends too early, and repeating one redeclares it — all three
        // are rejections, not mutants.
        const movable = s.type !== "VariableDeclaration"

        // Drop it. Never the only statement, so a body cannot empty out, and
        // never out of a loop, whose termination usually rests on it.
        if(movable && !scope.inLoop && source.length > 1 && ctx.rng.chance(ctx.rate * 0.25)) { ctx.applied++; continue }

        const mutated = mutateStatement(s, ctx, scope)

        if(movable && ctx.rng.chance(ctx.rate * 0.35)) { ctx.applied++; out.push(wrap(mutated, ctx, scope)) }
        else out.push(mutated)

        // Duplicate it — cheap way to reach a deeper operand stack and a
        // longer block than any seed holds.
        if(movable && ctx.rng.chance(ctx.rate * 0.15)) { ctx.applied++; out.push(mutateStatement(s, ctx, childScope(scope))) }
    }

    if(out.length === 0) out.push(sideEffect(ctx, scope))
    if(closer !== undefined) out.push(closer)
    return out
}

function mutateBody(list: readonly Statement[], ctx: Ctx, scope: Scope, inCase = false): Statement[]
{
    return mutateStatements(list, ctx, childScope(scope, inCase))
}

/** Does this clause run on into whatever is written after it? */
function fallsInto(c: {consequent: readonly Statement[]}): boolean
{
    const last = c.consequent[c.consequent.length - 1]
    if(last === undefined) return true
    if(last.type === "BreakStatement" || last.type === "ReturnStatement") return false
    return !(last.type === "ExpressionStatement" && last.expression.type === "CallExpression"
        && last.expression.callee.name === "trap")
}

function mutateStatement(s: Statement, ctx: Ctx, scope: Scope): Statement
{
    const expr = (e: Expression): Expression => mutateExpr(e, ctx, scope)

    switch(s.type)
    {
        case "BreakStatement": return s

        case "BlockStatement":
            return {type: "BlockStatement", body: mutateBody(s.body, ctx, scope)}

        case "IfStatement":
            return {type: "IfStatement", test: expr(s.test),
                    consequent: {type: "BlockStatement", body: mutateBody(asList(s.consequent), ctx, scope)},
                    alternate: s.alternate === null
                        ? (ctx.rng.chance(ctx.rate * 0.3)
                            ? {type: "BlockStatement", body: [sideEffect(ctx, childScope(scope))]}
                            : null)
                        : {type: "BlockStatement", body: mutateBody(asList(s.alternate), ctx, scope)}}

        case "WhileStatement":
        case "DoWhileStatement":
            // The condition is left alone: these come from seeds whose
            // termination depends on it, and rewriting it is the one easy
            // way to manufacture a program nothing can run.
            return {...s, body: {type: "BlockStatement",
                body: mutateStatements(asList(s.body), ctx, childScope(scope, false, true))}}

        case "ForStatement":
        {
            const inner = childScope(scope)
            if(s.init !== null && s.init.type === "VariableDeclaration")
                for(const d of s.init.declarations) inner.vars.push({name: d.id.name, type: d.varType})
            // Same reasoning as the while loops: init/test/update are the
            // loop's trip count and stay as written.
            inner.inLoop = true
            return {...s, body: {type: "BlockStatement", body: mutateStatements(asList(s.body), ctx, inner)}}
        }

        case "SwitchStatement":
        {
            // A case label is an integer literal by grammar and unique among
            // its siblings, so it is carried through untouched.
            const cases = s.cases.map(c => ({
                type: "SwitchCase" as const,
                test: c.test,
                consequent: mutateBody(c.consequent, ctx, scope, true),
            }))

            // Add a case. Its test is a literal distinct from the others, so
            // the table stays a table rather than collapsing on a duplicate.
            if(ctx.rng.chance(ctx.rate * 0.4))
            {
                ctx.applied++
                const used = new Set(cases.map(c => c.test?.type === "Literal" ? c.test.value : -1))
                let next = 0
                while(used.has(next)) next++
                const body: Statement[] = [sideEffect(ctx, childScope(scope, true)), {type: "BreakStatement"}]
                const at = cases.findIndex(c => c.test === null)
                const where = at < 0 ? cases.length : at
                // Never between a case that runs on and the one it runs into:
                // the new label lands in the middle of the chain and the
                // fall-through no longer names the next value.
                if(where === 0 || !fallsInto(cases[where - 1]!))
                    cases.splice(where, 0, {type: "SwitchCase" as const, test: lit(next), consequent: body})
            }

            // Drop a case's closer so it runs on into the next one. Legal
            // exactly where that next case is the default or is labelled one
            // higher (isa-core.md §4.5); a non-breaking default is an error.
            // Nothing else in the walk ever produces a fall-through, and the
            // `DEFAULT` closer is a translator path of its own.
            if(ctx.rng.chance(ctx.rate * 0.3))
            {
                const opens = cases.filter((c, at) =>
                {
                    const next = cases[at + 1]
                    if(next === undefined || c.test === null || c.consequent.length < 2) return false
                    if(fallsInto(c)) return false
                    if(next.test === null) return true
                    return next.test.type === "Literal" && c.test.type === "Literal"
                        && next.test.value === c.test.value + 1
                })
                const opened = ctx.rng.maybePick(opens)
                if(opened !== undefined)
                {
                    ctx.applied++
                    opened.consequent = opened.consequent.slice(0, -1)
                }
            }

            // Removing a case is only safe where nothing falls into it: a
            // clause with no `break` runs on into whatever is written next,
            // and lower.ts insists that be the case one label higher. A
            // switch also needs at least one labelled case.
            if(ctx.rng.chance(ctx.rate * 0.2))
            {
                const labelled = cases.filter(c => c.test !== null).length
                const removable = cases.filter((c, at) =>
                    (c.test === null || labelled > 1) && (at === 0 || !fallsInto(cases[at - 1]!)))
                const victim = ctx.rng.maybePick(removable)
                if(victim !== undefined) { ctx.applied++; cases.splice(cases.indexOf(victim), 1) }
            }

            return {type: "SwitchStatement", discriminant: expr(s.discriminant), cases}
        }

        case "VariableDeclaration":
        {
            const declarations = s.declarations.map(d => ({...d, init: d.init === null ? null : expr(d.init)}))
            for(const d of declarations) scope.vars.push({name: d.id.name, type: d.varType})
            if(ctx.rng.chance(ctx.rate * 0.3))
            {
                ctx.applied++
                // One declaration carries one type name, so every declarator
                // moves together or the statement has no surface form.
                const retyped = ctx.rng.pick(ALL_TYPES)
                return {type: "VariableDeclaration", declarations: declarations.map(d => ({...d, varType: retyped}))}
            }
            return {type: "VariableDeclaration", declarations}
        }

        case "ReturnStatement":
            // Never rewritten to a bare `return;` — a body that returns a
            // value on some paths and none on others has no signature.
            return {type: "ReturnStatement", argument: s.argument === null ? build(ctx, scope, 2) : expr(s.argument)}

        case "ExpressionStatement":
            return {type: "ExpressionStatement", expression: expr(s.expression)}
    }
}

const asList = (b: {type: string} & Statement): Statement[] =>
    b.type === "BlockStatement" ? [...b.body] : [b]

// ── whole programs ──────────────────────────────────────────────────────

/** Close a body so it returns a value on every path: `lowerProc` deduces
 *  the signature from the `return`s it finds, and an entry that establishes
 *  nothing has an unspecified result (isa-core.md §8.7). */
function closeWithReturn(body: Statement[], ctx: Ctx, scope: Scope): Statement[]
{
    const last = body[body.length - 1]
    if(last === undefined) return [{type: "ReturnStatement", argument: build(ctx, scope, 2)}]

    if(last.type === "ReturnStatement" && last.argument !== null) return body
    // `trap(...)` terminates too (lower.ts's `alwaysTerminates`), so anything
    // after one is unreachable and the validator says so.
    if(last.type === "ExpressionStatement" && last.expression.type === "CallExpression"
        && last.expression.callee.name === "trap") return body

    return [...body, {type: "ReturnStatement", argument: build(ctx, scope, 2)}]
}

/** Every expression in a program, as splice donors. */
function harvest(gen: GenProgram): Expression[]
{
    const out: Expression[] = []
    for(const p of gen.procs) eachExpression(p.body, e =>
    {
        if(e.type === "Identifier" || e.type === "Literal") return
        if(e.type === "CallExpression" && OPAQUE_CALLS.has(e.callee.name)) return
        out.push(e)
    })
    return out
}

export interface MutateOptions
{
    /** Per-site mutation probability. */
    rate?: number
    /** Extra splice donors, normally harvested from the rest of the corpus. */
    donors?: Expression[]
}

/** One mutant of `gen`. Deterministic in `seed`: the pair is the repro. */
export function mutate(gen: GenProgram, seed: number, options: MutateOptions = {}): GenProgram
{
    const rng = new Rng(seed)
    const rate = options.rate ?? 0.12

    let counter = 0
    const used = new Set<string>()
    for(const p of gen.procs)
    {
        for(const a of p.args) used.add(a)
        eachExpression(p.body, e => { if(e.type === "Identifier") used.add(e.name) })
        // Declarations too: one whose name nothing reads is in no expression
        // at all, and reusing it is a redeclaration.
        for(const name of declaredNames(p.body)) used.add(name)
    }
    const fresh = (): string =>
    {
        let name = `v${counter++}`
        while(used.has(name)) name = `v${counter++}`
        used.add(name)
        return name
    }

    const donors = [...harvest(gen), ...(options.donors ?? [])]

    const procs = gen.procs.map((p, i) =>
    {
        const ctx: Ctx = {
            rng, fresh, donors, rate, applied: 0,
            callable: gen.procs.slice(i + 1).map((c, k) => ({name: procName(i + 1 + k), arity: c.args.length})),
        }
        const scope: Scope = {vars: p.args.map(a => ({name: a, type: "u32" as PrimType})), inCase: false, inLoop: false}
        const body = mutateStatements(p.body, ctx, scope)
        return {args: [...p.args], body: closeWithReturn(body, ctx, scope)}
    })

    // Grow the graph: a new leaf procedure, called from somewhere that may
    // legally reach it. Appended last, so every existing edge still points
    // upward and the DAG holds without renumbering anything.
    if(rng.chance(0.15) && procs.length < 8)
    {
        const index = procs.length
        const arity = rng.between(0, 4)
        const args = Array.from({length: arity}, () => fresh())
        const ctx: Ctx = {rng, fresh, donors, rate, applied: 0, callable: []}
        const scope: Scope = {vars: args.map(a => ({name: a, type: "u32" as PrimType})), inCase: false, inLoop: false}
        procs.push({args, body: [{type: "ReturnStatement", argument: build(ctx, scope, 2)}]})

        // Called as a bare statement, which needs nowhere to put the
        // result and so is legal in any caller.
        const from = rng.int(index)
        const caller = procs[from]!
        const callerCtx: Ctx = {rng, fresh, donors, rate, applied: 0, callable: []}
        const callerScope: Scope = {vars: caller.args.map(a => ({name: a, type: "u32" as PrimType})), inCase: false, inLoop: false}
        caller.body.unshift({type: "ExpressionStatement", expression: {
            type: "CallExpression", callee: ident(procName(index)),
            arguments: Array.from({length: arity}, () => build(callerCtx, callerScope, 1)),
        }})
    }

    return {procs}
}

/** A mutant whose operands are all sequenced, found by re-rolling rather
 *  than by steering the walk away from writes — a mutation that races a
 *  sibling is only visible once the whole expression exists, and re-rolling
 *  costs a fraction of what lowering the mutant would.
 *
 *  Deterministic in `seed`: the derived attempts are a fixed sequence, so
 *  `(corpus entry, seed)` still identifies exactly one program. */
export function mutateSequenced(gen: GenProgram, seed: number, options: MutateOptions = {}): GenProgram | undefined
{
    for(let attempt = 0; attempt < 6; attempt++)
    {
        const candidate = mutate(gen, (Math.imul(seed, 0x9e3779b1) + attempt) >>> 0, options)
        if(!analyze(candidate).unsequenced) return candidate
    }
    return undefined
}

/** A body long enough that a branch across it does not reach.
 *
 *  A conditional branch spans ±254 bytes and the wide form the translator
 *  retries in spans ±2046 (translate_control_flow.cpp's `emitBranch`).
 *  Ordinary mutation adds a statement at a time against a corpus bounded far
 *  below either, so the translator's out-of-range paths cannot be reached
 *  from it at all. This builds the shape directly: one construct with no
 *  `else` over a long straight-line run, so the branch across it — forward
 *  out of an `if`, backward off a loop — has to clear the whole thing. */
export function inflate(gen: GenProgram, seed: number, statements: number): GenProgram
{
    const rng = new Rng(seed)

    const used = new Set<string>()
    for(const p of gen.procs)
    {
        for(const a of p.args) used.add(a)
        eachExpression(p.body, e => { if(e.type === "Identifier") used.add(e.name) })
        for(const name of declaredNames(p.body)) used.add(name)
    }
    let counter = 0
    const fresh = (): string =>
    {
        let name = `w${counter++}`
        while(used.has(name)) name = `w${counter++}`
        used.add(name)
        return name
    }

    const entry = gen.procs[0]!
    const ctx: Ctx = {rng, fresh, donors: [], rate: 0, applied: 0, callable: []}
    const outer: Scope = {vars: entry.args.map(a => ({name: a, type: "u32" as PrimType})), inCase: false, inLoop: false}

    // Locals for the run to assign to. Without them every padding statement
    // is a store, which reaches only the extension and costs more bytecode
    // per byte of code than the plain arithmetic this lane wants.
    const prologue: Statement[] = []
    for(let i = 0; i < 3; i++) prologue.push(declare(ctx, outer))

    const inner = childScope(outer)
    const pad: Statement[] = []
    for(let i = 0; i < statements; i++) pad.push(sideEffect(ctx, inner))

    const span: Statement = [
        (): Statement => ({type: "IfStatement", test: build(ctx, outer, 1),
                           consequent: {type: "BlockStatement", body: pad}, alternate: null}),
        (): Statement => countedLoop(pad, ctx, outer),
        (): Statement => countedDoWhile(pad, ctx, outer),
    ][rng.int(3)]!()

    return {procs: [
        {args: [...entry.args], body: closeWithReturn([...prologue, span, ...entry.body], ctx, outer)},
        ...gen.procs.slice(1),
    ]}
}

/** {@link inflate} under the same sequencing filter {@link mutateSequenced}
 *  applies. */
export function inflateSequenced(gen: GenProgram, seed: number, statements: number): GenProgram | undefined
{
    for(let attempt = 0; attempt < 6; attempt++)
    {
        const candidate = inflate(gen, (Math.imul(seed, 0x85ebca6b) + attempt) >>> 0, statements)
        if(!analyze(candidate).unsequenced) return candidate
    }
    return undefined
}

/** Whether `gen` calls anything at all — used by the driver to report how
 *  much of the corpus reaches the call path. */
export function callDepthReached(gen: GenProgram): boolean
{
    return gen.procs.some(p => calleesOf(p.body).length > 0)
}

/** Statement-nesting depth, one of the retention signature's components. */
export function nestDepth(list: readonly Statement[]): number
{
    let deepest = 0
    for(const s of list)
    {
        for(const nested of childBodies(s)) deepest = Math.max(deepest, 1 + nestDepth(nested))
    }
    return deepest
}
