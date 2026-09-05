// fuzz — shrink a finding to the smallest program that still shows it.
//
//     npx ts-node --transpile-only fuzz/ts/gen/minimize.ts <program.json> [--jit] [-o out.json]
//
// Works on the tree rather than on instructions or bytes, so what comes out
// is readable DSL source — the form a person reasons about — and every
// candidate it tries is well-formed by construction rather than being
// garbage that happens to decode.
//
// Two predicates:
//
//   default  the AST evaluator and the reference VM disagree (a lowerer or
//            VM bug) — in process, so a pass costs milliseconds
//   --jit    the emitted Thumb disagrees with the reference VM — one QEMU
//            boot per candidate, which is affordable here because a boot on
//            this model is a fraction of a second
//
// `minimize-exec.ts` is the instruction-level counterpart, for a finding
// that arrives as an encoded program rather than as a `(entry, seed)` pair.

import { encodeJitProgram, run, validateProgram, StepLimitExceeded, UnspecifiedShiftAmount } from "mog-core"
import type { Statement } from "mog-core"
import { rawMemExtension } from "../lib/rawmem_ext"
import { entryArgsFor } from "../lib/entry_args"
import { digestOf, parseResults, runQemu, writeBatch } from "../lib/batch"
import { lowerGen } from "./corpus"
import type { GenProgram } from "./corpus"
import { loadProgram, saveProgram } from "./program_file"
import { print } from "./print"
import { analyze } from "./ub"
import { childBodies } from "./walk"
import { evaluate } from "../eval/ast-eval"
import * as path from "path"

const EXT = rawMemExtension()
const ELF = path.join(__dirname, "..", "..", "src", "qemu-exec", "exec_runner.elf")
const BATCH_PATH = `/tmp/ppl-fuzz-minimize-${process.pid}.bin`
const MAX_STEPS = 200_000

const JIT = process.argv.includes("--jit")

/** Does `gen` still show the finding? Anything that no longer lowers, no
 *  longer terminates or has become unsequenced is not a smaller witness —
 *  it is a different program. */
function stillFails(gen: GenProgram): boolean
{
    if(analyze(gen).unsequenced) return false

    let rtl
    // No round trip: shrinking runs the whole pipeline per candidate and
    // parsing is the bulk of it, while every reduction here is structural —
    // the tree came from a program that had already been through the parser.
    try { rtl = lowerGen(gen, EXT, false); validateProgram(rtl, EXT) }
    catch { return false }

    const args = entryArgsFor(rtl.procedures[0]!.argCount)

    EXT.reset()
    const ast = evaluate(gen, args, EXT, MAX_STEPS)
    const astDigest = digestOf(EXT.mem)
    if(ast.outcome.kind === "unspecified" || ast.outcome.kind === "steplimit") return false

    EXT.reset()
    let vm
    try { vm = run(rtl, EXT, args, MAX_STEPS) }
    catch(e) { return !(e instanceof StepLimitExceeded || e instanceof UnspecifiedShiftAmount) }
    const vmDigest = digestOf(EXT.mem)

    // A void entry establishes no result (isa-core.md §8.7), so the two
    // sides are free to differ and a shrink that reaches one has found a
    // different program rather than a smaller witness.
    if(vm.ok && !vm.accLive) return false

    const reference = ast.outcome.kind === "return"
        ? {kind: "return" as const, value: ast.outcome.value}
        : {kind: "trap" as const, code: ast.outcome.code}

    const agree = reference.kind === "return"
        ? vm.ok && (vm.acc >>> 0) === reference.value
        : !vm.ok && ((vm.trapCode ?? 0) >>> 0) === reference.code

    if(!JIT) return !agree || astDigest !== vmDigest

    // The reference side has to be self-consistent before the target's
    // answer means anything.
    if(!agree || astDigest !== vmDigest) return false

    let bytes
    try { bytes = Buffer.from(encodeJitProgram(rtl, EXT)) }
    catch { return false }

    writeBatch(BATCH_PATH, [{bytes, entryArgs: args}])
    const r = runQemu(ELF, BATCH_PATH, 20_000)
    if(r.timedOut) return true // a hang is a finding too

    const [result] = parseResults(r.output)
    if(result === undefined || result.kind === "X" || result.kind === "E") return false

    const targetAgrees = reference.kind === "return"
        ? result.kind === "R" && result.value === reference.value
        : result.kind === "T" && result.value === reference.code

    return !targetAgrees || (result.digest !== null && result.digest !== vmDigest)
}

// ── the reductions ──────────────────────────────────────────────────────

const clone = (gen: GenProgram): GenProgram =>
    JSON.parse(JSON.stringify(gen)) as GenProgram

/** Nodes in the tree. Every accepted reduction has to lower this, which is
 *  what makes the loop terminate: without it, replacing a `Literal 0` with
 *  a `Literal 0` counts as progress and the pass never ends. */
function size(node: unknown): number
{
    if(Array.isArray(node)) return node.reduce<number>((n, c) => n + size(c), 0)
    if(node === null || typeof node !== "object") return 0
    return 1 + Object.values(node as Record<string, unknown>).reduce<number>((n, c) => n + size(c), 0)
}

/** Every way of removing one statement from `list`, in place. */
function* withoutOne(list: Statement[]): Generator<Statement[]>
{
    for(let i = 0; i < list.length; i++) yield [...list.slice(0, i), ...list.slice(i + 1)]
}

/** Every statement list in the program, addressed so it can be replaced. */
function lists(gen: GenProgram): {get(): Statement[]; set(v: Statement[]): void}[] 
{
    const out: {get(): Statement[]; set(v: Statement[]): void}[] = []

    const walk = (owner: {body: Statement[]}): void =>
    {
        out.push({get: () => owner.body, set: v => { owner.body = v }})
        for(const s of owner.body)
        {
            for(const nested of childBodies(s))
            {
                // `childBodies` hands back the real arrays, so writing into
                // one is how a nested body is replaced.
                walk({get body() { return nested }, set body(v) { nested.length = 0; nested.push(...v) }})
            }
        }
    }

    for(const p of gen.procs) walk(p)
    return out
}

const EXPRESSION_TYPES = new Set([
    "CastExpression", "AssignmentExpression", "ConditionalExpression", "LogicalExpression",
    "BinaryExpression", "UnaryExpression", "UpdateExpression", "CallExpression", "Literal", "Identifier",
])

type Path = (string | number)[]

/** Every expression-shaped node in the tree, by the path that reaches it. */
function expressionPaths(node: unknown, at: Path = [], out: Path[] = []): Path[]
{
    if(Array.isArray(node))
    {
        node.forEach((child, i) => expressionPaths(child, [...at, i], out))
        return out
    }
    if(node === null || typeof node !== "object") return out

    const record = node as Record<string, unknown>
    if(typeof record.type === "string" && EXPRESSION_TYPES.has(record.type) && at.length > 0) out.push(at)
    for(const key of Object.keys(record)) expressionPaths(record[key], [...at, key], out)
    return out
}

const getAt = (root: unknown, at: Path): unknown =>
    at.reduce<unknown>((node, key) => (node as Record<string | number, unknown>)[key], root)

function setAt(root: unknown, at: Path, value: unknown): void
{
    const parent = getAt(root, at.slice(0, -1)) as Record<string | number, unknown>
    parent[at[at.length - 1]!] = value
}

const lit = (value: number): unknown => ({type: "Literal", value, raw: String(value)})

/** What one expression could be replaced by and still mean something: any
 *  expression-typed child, or a bare constant. A replacement that is not
 *  legal in this position — a literal where an lvalue belongs, a
 *  non-literal case label — simply fails to lower and is discarded. */
function replacements(node: unknown): unknown[]
{
    const out: unknown[] = [lit(0), lit(1)]
    const record = node as Record<string, unknown>
    for(const key of Object.keys(record))
    {
        const child = record[key]
        if(child !== null && typeof child === "object" && !Array.isArray(child)
            && EXPRESSION_TYPES.has((child as {type?: string}).type ?? "")) out.push(child)
    }
    return out
}

function shrink(gen: GenProgram): GenProgram
{
    let best = gen
    let progress = true
    let tried = 0

    while(progress)
    {
        progress = false

        // Whole procedures first: dropping one takes everything under it.
        for(let i = best.procs.length - 1; i >= 1; i--)
        {
            const candidate = clone(best)
            candidate.procs.splice(i, 1)
            tried++
            const before_size = size(best)
            // Only valid if nothing still calls it, which `checkGraph`
            // decides for us by way of `stillFails` failing to lower.
            if(stillFails(candidate)) { best = candidate; progress = true }
        }

        for(let at = 0; at < lists(best).length; at++)
        {
            const before = lists(best)[at]!.get()
            if(before.length === 0) continue

            const before_size = size(best)
            for(const shorter of withoutOne(before))
            {
                const candidate = clone(best)
                lists(candidate)[at]!.set(shorter)
                tried++
                if(size(candidate) < before_size && stillFails(candidate)) { best = candidate; progress = true; break }
            }
        }

        // Deepest first, so collapsing an inner node does not invalidate the
        // paths still to be tried above it.
        const paths = expressionPaths(best).sort((a, b) => b.length - a.length)
        for(const at of paths)
        {
            const node = getAt(best, at)
            if(node === undefined) continue

            const before_size = size(best)
            for(const smaller of replacements(node))
            {
                const candidate = clone(best)
                setAt(candidate, at, JSON.parse(JSON.stringify(smaller)))
                tried++
                if(size(candidate) < before_size && stillFails(candidate)) { best = candidate; progress = true; break }
            }
        }
    }

    console.error(`${tried} candidate(s) tried`)
    return best
}

// ── main ────────────────────────────────────────────────────────────────

const positional = process.argv.slice(2).filter(a => !a.startsWith("-"))
const input = positional[0]
if(input === undefined)
{
    console.error("usage: minimize.ts <program.json> [--jit] [-o out.json]")
    process.exit(1)
}

const start = loadProgram(input)

if(!stillFails(start))
{
    console.error(`${input} does not fail the ${JIT ? "--jit" : "default"} predicate — nothing to shrink`)
    process.exit(1)
}

const result = shrink(start)
console.log(result.procs.map((p, i) => `--- p${i}(${p.args.join(", ")}) ---\n${print(p.body)}`).join(""))

const at = process.argv.indexOf("-o")
if(at > 0 && process.argv[at + 1] !== undefined)
{
    saveProgram(process.argv[at + 1]!, result)
    console.error(`wrote ${process.argv[at + 1]}`)
}
