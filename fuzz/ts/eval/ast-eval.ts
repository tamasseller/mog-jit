// fuzz — the third engine: the DSL evaluated directly, as written.
//
// Its whole value is being INDEPENDENT. It restates the language contract
// rather than reusing the implementation of it, so nothing in here may
// import `desugar`, `types.ts`'s `annotate`, or `vm.ts`'s
// `evalBinary`/`evalUnary` — a bug in any of those would otherwise sit on
// both sides of the comparison and cancel out. The type and signedness
// rules below are written from isa-core.md and types.ts's stated rule, not
// called into.
//
// The boundary is the extension: `rawmem_ext.ts` *is* the specification of
// what those opcodes mean, so its own `exec` is driven here through a small
// `ExecState` adapter. Reimplementing MEMMOVE a second time would invent a
// disagreement rather than detect one.
//
// Two things are deliberately not modelled, because the driver drops them
// before any comparison: a program that does not terminate (the step limit
// catches it) and one whose operands are unsequenced (ub.ts catches it).

import type {
    ControlBody, Expression, PrimType, Statement, SwitchCase,
} from "mog-core"
import type { ExecState, Extension } from "mog-core"
import type { GenProc, GenProgram } from "../gen/corpus"

// ── the type rule ───────────────────────────────────────────────────────
//
// C's, over this six-type menu: everything narrower than a word promotes to
// `i32`, so `u32` alone survives promotion unsigned. An operation is
// unsigned exactly when an operand promotes to `u32`. Shifts are the
// asymmetry — the usual arithmetic conversions do not apply to them, so the
// left operand alone decides.

const promote = (t: PrimType): "u32" | "i32" => t === "u32" ? "u32" : "i32"

const usual = (a: PrimType, b: PrimType): PrimType =>
    promote(a) === "u32" || promote(b) === "u32" ? "u32" : "i32"

const COMPARISONS = new Set(["==", "!=", "<", "<=", ">", ">="])

/** A narrow type is stored already extended, so every write through one
 *  goes through here and every read is the extended word. */
function narrow(value: number, type: PrimType): number
{
    switch(type)
    {
        case "u8": return value & 0xff
        case "u16": return value & 0xffff
        case "i8": return ((value << 24) >> 24) >>> 0
        case "i16": return ((value << 16) >> 16) >>> 0
        default: return value >>> 0
    }
}

/** An unsuffixed literal is an `int` unless it does not fit one. */
const literalType = (value: number): PrimType => value <= 0x7fffffff ? "i32" : "u32"

// ── outcomes ────────────────────────────────────────────────────────────

export type Outcome =
    | {kind: "return"; value: number}
    | {kind: "trap"; code: number}
    /** The program's answer is not fixed by the language — a shift amount of
     *  32 or more (isa-core.md §4.1). Nothing to compare. */
    | {kind: "unspecified"; reason: string}
    | {kind: "steplimit"}

class Trapped extends Error { constructor(readonly code: number) { super(`trap ${code}`) } }
class Unspecified extends Error { constructor(readonly reason: string) { super(reason) } }
class StepLimit extends Error {}

// ── environment ─────────────────────────────────────────────────────────

interface Slot { type: PrimType; value: number }

class Scope
{
    private readonly vars = new Map<string, Slot>()

    constructor(private readonly parent?: Scope) {}

    declare(name: string, type: PrimType, value: number): void
    {
        this.vars.set(name, {type, value: narrow(value, type)})
    }

    lookup(name: string): Slot
    {
        const found = this.vars.get(name) ?? this.parent?.lookup(name)
        if(found === undefined) throw new Error(`ast-eval: unknown variable '${name}'`)
        return found
    }

    assign(name: string, value: number): number
    {
        const slot = this.lookup(name)
        slot.value = narrow(value, slot.type)
        return slot.value
    }
}

// ── the extension seam ──────────────────────────────────────────────────

const MEM_STACK_ARITY: Readonly<Record<string, number>> = {
    st8: 1, st16: 1, st32: 1, memmove: 3, memcmp: 3, slicecmp: 4,
}
const MEM_ACC_OPS = new Set(["ld8", "ld16", "ld32", "st8", "st16", "st32"])
const EXT_OPS = new Set([...Object.keys(MEM_STACK_ARITY), "ld8", "ld16", "ld32"])

/** Run one extension opcode through the extension's own `exec`, which is
 *  the specification. `stack` is in push order, so `pop` sees it reversed —
 *  the same order the emitted code establishes. */
function runExt(extension: Extension, name: string, stack: number[], acc: number): number
{
    const cells = stack.map(v => v >>> 0)
    const state: ExecState = {
        acc: acc >>> 0,
        push(value: number) { cells.push(value >>> 0) },
        pop(): number
        {
            const value = cells.pop()
            if(value === undefined) throw new Error(`ast-eval: ${name} popped an empty stack`)
            return value
        },
        reg() { throw new Error(`ast-eval: ${name} read a register`) },
        setReg() { throw new Error(`ast-eval: ${name} wrote a register`) },
        callProc() { throw new Error(`ast-eval: ${name} called a procedure`) },
    }

    extension.exec?.({op: "EXT", ext: name.toUpperCase(), operands: []}, state)
    return state.acc >>> 0
}

// ── evaluation ──────────────────────────────────────────────────────────

interface Val { value: number; type: PrimType }

type Flow =
    | {kind: "normal"}
    | {kind: "return"; value: number}
    | {kind: "break"}

const NORMAL: Flow = {kind: "normal"}

const asList = (b: ControlBody): Statement[] => b.type === "BlockStatement" ? b.body : [b]

class Machine
{
    steps = 0

    constructor(
        private readonly procs: readonly GenProc[],
        private readonly extension: Extension,
        private readonly maxSteps: number) {}

    private tick(): void
    {
        if(++this.steps > this.maxSteps) throw new StepLimit()
    }

    // ── expressions ─────────────────────────────────────────────────────

    /** The static type of `e`, without evaluating it — needed where a type
     *  depends on a subtree the evaluation does not take (a ternary's
     *  untaken arm). */
    private typeOf(e: Expression, scope: Scope): PrimType
    {
        switch(e.type)
        {
            case "Literal": return literalType(e.value)
            case "Identifier": return scope.lookup(e.name).type
            case "CastExpression": return e.varType
            case "LogicalExpression": return "i32"
            case "CallExpression": return "u32"
            case "AssignmentExpression": return scope.lookup(e.left.name).type
            case "UpdateExpression":
                return e.argument.type === "Identifier" ? scope.lookup(e.argument.name).type : "u32"
            case "UnaryExpression":
                return e.operator === "!" ? "i32" : promote(this.typeOf(e.argument, scope))
            case "ConditionalExpression":
                return usual(this.typeOf(e.consequent, scope), this.typeOf(e.alternate, scope))
            case "BinaryExpression":
            {
                if(COMPARISONS.has(e.operator)) return "i32"
                const left = this.typeOf(e.left, scope)
                return e.operator === "<<" || e.operator === ">>"
                    ? promote(left)
                    : usual(left, this.typeOf(e.right, scope))
            }
        }
    }

    private binary(operator: string, left: Val, right: Val): Val
    {
        const a = left.value >>> 0
        const b = right.value >>> 0

        // Shifts take their signedness from the left operand alone; every
        // other operator from the usual arithmetic conversions.
        const shift = operator === "<<" || operator === ">>"
        const signed = shift
            ? promote(left.type) === "i32"
            : promote(left.type) === "i32" && promote(right.type) === "i32"

        const type: PrimType = COMPARISONS.has(operator) ? "i32"
            : shift ? promote(left.type) : usual(left.type, right.type)

        const of = (value: number): Val => ({value: value >>> 0, type})

        switch(operator)
        {
            case "+": return of(a + b)
            case "-": return of(a - b)
            case "*": return of(Math.imul(a, b))
            case "&": return of(a & b)
            case "|": return of(a | b)
            case "^": return of(a ^ b)

            case "<<": case ">>":
            {
                // isa-core.md §4.1 defines 0..31 and nothing else.
                if(b > 31) throw new Unspecified(`shift by ${b}`)
                if(operator === "<<") return of(a << b)
                return of(signed ? (a | 0) >> b : a >>> b)
            }

            case "==": return of(a === b ? 1 : 0)
            case "!=": return of(a !== b ? 1 : 0)
            case "<": return of((signed ? (a | 0) < (b | 0) : a < b) ? 1 : 0)
            case "<=": return of((signed ? (a | 0) <= (b | 0) : a <= b) ? 1 : 0)
            case ">": return of((signed ? (a | 0) > (b | 0) : a > b) ? 1 : 0)
            case ">=": return of((signed ? (a | 0) >= (b | 0) : a >= b) ? 1 : 0)

            default: throw new Error(`ast-eval: no such binary operator '${operator}'`)
        }
    }

    private call(name: string, args: Expression[], scope: Scope): Val
    {
        const u32 = (value: number): Val => ({value: value >>> 0, type: "u32"})

        if(name === "trap") throw new Trapped(this.eval(args[0]!, scope).value >>> 0)

        if(name === "clz") return u32(Math.clz32(this.eval(args[0]!, scope).value >>> 0))
        if(name === "revbits")
        {
            let x = this.eval(args[0]!, scope).value >>> 0
            x = ((x & 0x55555555) << 1) | ((x >>> 1) & 0x55555555)
            x = ((x & 0x33333333) << 2) | ((x >>> 2) & 0x33333333)
            x = ((x & 0x0f0f0f0f) << 4) | ((x >>> 4) & 0x0f0f0f0f)
            x = ((x & 0x00ff00ff) << 8) | ((x >>> 8) & 0x00ff00ff)
            return u32(((x << 16) | (x >>> 16)) >>> 0)
        }

        if(EXT_OPS.has(name))
        {
            const values = args.map(a => this.eval(a, scope).value >>> 0)
            // A load addresses through acc; a store takes its address off
            // the stack and its value from acc; the rest are all stack.
            if(!MEM_ACC_OPS.has(name)) return u32(runExt(this.extension, name, values, 0))
            if(name.startsWith("ld")) return u32(runExt(this.extension, name, [], values[0]!))
            return u32(runExt(this.extension, name, [values[0]!], values[1]!))
        }

        const match = /^p([0-9]+)$/.exec(name)
        if(match === null) throw new Error(`ast-eval: no such callee '${name}'`)

        // Arguments are `u32` (isa-core.md §2.3), so nothing narrows here.
        const values = args.map(a => this.eval(a, scope).value >>> 0)
        return u32(this.runProc(Number(match[1]), values))
    }

    private eval(e: Expression, scope: Scope): Val
    {
        this.tick()

        switch(e.type)
        {
            case "Literal": return {value: e.value >>> 0, type: literalType(e.value)}
            case "Identifier": { const s = scope.lookup(e.name); return {value: s.value, type: s.type} }

            case "CastExpression":
            {
                const inner = this.eval(e.argument, scope)
                return {value: narrow(inner.value, e.varType), type: e.varType}
            }

            case "UnaryExpression":
            {
                const arg = this.eval(e.argument, scope)
                switch(e.operator)
                {
                    case "-": return {value: (-arg.value) >>> 0, type: promote(arg.type)}
                    case "~": return {value: (~arg.value) >>> 0, type: promote(arg.type)}
                    case "!": return {value: arg.value === 0 ? 1 : 0, type: "i32"}
                    default: return {value: arg.value >>> 0, type: promote(arg.type)}
                }
            }

            case "BinaryExpression":
                return this.binary(e.operator, this.eval(e.left, scope), this.eval(e.right, scope))

            case "LogicalExpression":
            {
                // Short-circuit, and the result is a boolean `int`.
                const left = this.eval(e.left, scope).value >>> 0
                if(e.operator === "&&" && left === 0) return {value: 0, type: "i32"}
                if(e.operator === "||" && left !== 0) return {value: 1, type: "i32"}
                return {value: this.eval(e.right, scope).value !== 0 ? 1 : 0, type: "i32"}
            }

            case "ConditionalExpression":
            {
                // Only the taken arm runs, but the value's type is what the
                // two arms convert to, so the other one is still typed.
                const type = usual(this.typeOf(e.consequent, scope), this.typeOf(e.alternate, scope))
                const taken = this.eval(e.test, scope).value !== 0 ? e.consequent : e.alternate
                return {value: this.eval(taken, scope).value >>> 0, type}
            }

            case "AssignmentExpression":
            {
                const slot = scope.lookup(e.left.name)
                const right = this.eval(e.right, scope)
                const value = e.operator === "="
                    ? right.value
                    : this.binary(e.operator.slice(0, -1), {value: slot.value, type: slot.type}, right).value
                return {value: scope.assign(e.left.name, value), type: slot.type}
            }

            case "UpdateExpression":
            {
                if(e.argument.type !== "Identifier") throw new Error("ast-eval: update of a non-variable")
                const slot = scope.lookup(e.argument.name)
                const before = slot.value
                const after = scope.assign(e.argument.name, e.operator === "++" ? before + 1 : before - 1)
                return {value: e.prefix ? after : before, type: slot.type}
            }

            case "CallExpression": return this.call(e.callee.name, e.arguments, scope)
        }
    }

    // ── statements ──────────────────────────────────────────────────────

    private block(list: readonly Statement[], parent: Scope): Flow
    {
        return this.run(list, new Scope(parent))
    }

    private body(b: ControlBody, scope: Scope): Flow
    {
        return this.block(asList(b), scope)
    }

    /** A switch runs the matching clause and then, unless that clause ended
     *  in `break`, continues into the next one *in source order* — C's rule,
     *  which lower.ts's `caseCloser` implements and constrains. */
    private switchOn(discriminant: number, cases: readonly SwitchCase[], scope: Scope): Flow
    {
        let at = cases.findIndex(c => c.test !== null && this.eval(c.test, scope).value >>> 0 === discriminant)
        if(at < 0) at = cases.findIndex(c => c.test === null)
        if(at < 0) return NORMAL

        for(let i = at; i < cases.length; i++)
        {
            const clause = cases[i]!
            const flow = this.block(clause.consequent, scope)
            if(flow.kind === "return") return flow
            if(flow.kind === "break") return NORMAL
            // Ran off the end of the clause: fall through into the next.
        }
        return NORMAL
    }

    private statement(s: Statement, scope: Scope): Flow
    {
        this.tick()

        switch(s.type)
        {
            case "BlockStatement": return this.block(s.body, scope)
            case "BreakStatement": return {kind: "break"}

            case "ExpressionStatement": this.eval(s.expression, scope); return NORMAL

            case "ReturnStatement":
                return {kind: "return", value: s.argument === null ? 0 : this.eval(s.argument, scope).value >>> 0}

            case "VariableDeclaration":
                for(const d of s.declarations)
                    scope.declare(d.id.name, d.varType, d.init === null ? 0 : this.eval(d.init, scope).value)
                return NORMAL

            case "IfStatement":
                if(this.eval(s.test, scope).value !== 0) return this.body(s.consequent, scope)
                return s.alternate === null ? NORMAL : this.body(s.alternate, scope)

            case "WhileStatement":
                for(;;)
                {
                    this.tick()
                    if(this.eval(s.test, scope).value === 0) return NORMAL
                    const flow = this.body(s.body, scope)
                    if(flow.kind === "return") return flow
                    if(flow.kind === "break") return NORMAL
                }

            case "DoWhileStatement":
                for(;;)
                {
                    this.tick()
                    const flow = this.body(s.body, scope)
                    if(flow.kind === "return") return flow
                    if(flow.kind === "break") return NORMAL
                    if(this.eval(s.test, scope).value === 0) return NORMAL
                }

            case "ForStatement":
            {
                // The init's declaration belongs to the loop, not to the
                // enclosing block.
                const outer = new Scope(scope)
                if(s.init !== null)
                {
                    if(s.init.type === "VariableDeclaration") this.statement(s.init, outer)
                    else this.eval(s.init, outer)
                }
                for(;;)
                {
                    this.tick()
                    if(s.test !== null && this.eval(s.test, outer).value === 0) return NORMAL
                    const flow = this.body(s.body, outer)
                    if(flow.kind === "return") return flow
                    if(flow.kind === "break") return NORMAL
                    if(s.update !== null) this.eval(s.update, outer)
                }
            }

            case "SwitchStatement":
                return this.switchOn(this.eval(s.discriminant, scope).value >>> 0, s.cases, scope)
        }
    }

    private run(list: readonly Statement[], scope: Scope): Flow
    {
        for(const s of list)
        {
            const flow = this.statement(s, scope)
            if(flow.kind !== "normal") return flow
        }
        return NORMAL
    }

    runProc(index: number, args: readonly number[], isEntry = false): number
    {
        const proc = this.procs[index]
        if(proc === undefined) throw new Error(`ast-eval: no procedure p${index}`)

        const scope = new Scope()
        proc.args.forEach((name, i) => scope.declare(name, "u32", args[i] ?? 0))

        const flow = this.run(proc.body, scope)
        if(flow.kind === "return") return flow.value >>> 0

        // Ran off its end. What comes back is whatever `acc` happens to
        // hold, which is a property of the emitted code and not of the
        // program — the last argument for an empty body (§4.6 puts it
        // there), nothing at all after a branch (§8.7 destroys acc at a CFG
        // split), and the operand of whatever ran last otherwise. §8.7 calls
        // that unspecified, and so does this.
        //
        // Only the entry procedure's own fall-through can be observed: the
        // DSL refuses to use a void call as a value, so a callee that ends
        // this way never hands anything to anyone.
        if(isEntry) throw new Unspecified("entry procedure runs off its end without a value")
        return 0
    }
}

export interface EvalResult { outcome: Outcome; steps: number }

export function evaluate(
    gen: GenProgram,
    args: readonly number[],
    extension: Extension,
    maxSteps = 200_000): EvalResult
{
    const machine = new Machine(gen.procs, extension, maxSteps)
    try
    {
        return {outcome: {kind: "return", value: machine.runProc(0, args, true)}, steps: machine.steps}
    }
    catch(e)
    {
        if(e instanceof Trapped) return {outcome: {kind: "trap", code: e.code}, steps: machine.steps}
        if(e instanceof Unspecified) return {outcome: {kind: "unspecified", reason: e.reason}, steps: machine.steps}
        if(e instanceof StepLimit) return {outcome: {kind: "steplimit"}, steps: machine.steps}
        throw e
    }
}
