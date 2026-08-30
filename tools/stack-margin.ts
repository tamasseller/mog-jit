#!/usr/bin/env -S node --experimental-strip-types --no-warnings

import { readFileSync, readdirSync, statSync } from 'node:fs'
import { join } from 'node:path'

type Edge = { target: string, loc: string }

type Node = {
    title: string
    file: string
    sig: string
    loc: string
    frame: number
    dynamicFrame: boolean
    dynamicObjects: number
    defined: boolean
    edges: Edge[]
}

const INDIRECT = '__indirect_call'

function field(line: string, key: string): string | null
{
    const m = new RegExp(`\\b${key}\\s*:\\s*"((?:[^"\\\\]|\\\\.)*)"`).exec(line)
    return m ? m[1] : null
}

function bareName(title: string): string
{
    const i = title.lastIndexOf(':')
    return i < 0 ? title : title.slice(i + 1)
}

/* Itanium ABI emits one body per ctor/dtor and aliases the other variants
 * onto it; only the body appears as a node. */
function aliases(name: string): string[]
{
    const m = /([CD])([0-3])E/.exec(name)
    if(!m) return [name]
    return ['0', '1', '2', '3'].map(d => name.replace(/([CD])([0-3])E/, `${m[1]}${d}E`))
}

function parse(path: string): Map<string, Node>
{
    const nodes = new Map<string, Node>()

    const at = (title: string): Node =>
    {
        let n = nodes.get(title)
        if(!n)
        {
            n = { title, file: path, sig: title, loc: '?', frame: 0, dynamicFrame: false, dynamicObjects: 0, defined: false, edges: [] }
            nodes.set(title, n)
        }
        return n
    }

    for(const line of readFileSync(path, 'utf8').split('\n'))
    {
        if(line.startsWith('node:'))
        {
            const title = field(line, 'title')
            if(title === null) continue

            const n = at(title)
            const parts = (field(line, 'label') ?? '').split('\\n')
            n.sig = parts[0] ?? title
            n.loc = parts[1] ?? '?'

            for(const p of parts)
            {
                const f = /^(\d+) bytes \((static|dynamic)\)$/.exec(p)
                if(f)
                {
                    n.defined = true
                    n.frame = parseInt(f[1], 10)
                    n.dynamicFrame = f[2] === 'dynamic'
                }

                const d = /^(\d+) dynamic objects$/.exec(p)
                if(d) n.dynamicObjects = parseInt(d[1], 10)
            }
        }
        else if(line.startsWith('edge:'))
        {
            const src = field(line, 'sourcename')
            const dst = field(line, 'targetname')
            if(src === null || dst === null) continue
            at(src).edges.push({ target: dst, loc: field(line, 'label') ?? '?' })
        }
    }

    return nodes
}

function collect(paths: string[]): string[]
{
    const out: string[] = []

    for(const p of paths)
    {
        if(statSync(p).isDirectory())
        {
            out.push(...collect(readdirSync(p).map(e => join(p, e))))
        }
        else if(p.endsWith('.ci'))
        {
            out.push(p)
        }
    }

    return out
}

const argv = process.argv.slice(2)
let guardPattern: string | null = null
let max = Infinity

const g = argv.indexOf('--guard')
if(g >= 0)
{
    guardPattern = argv[g + 1] ?? null
    argv.splice(g, 2)
}

const m = argv.indexOf('--max')
if(m >= 0)
{
    max = Number(argv[m + 1])
    argv.splice(m, 2)
}

const [pattern, ...roots] = argv

if(!pattern || roots.length === 0 || (g >= 0 && guardPattern === null) || !(max > 0))
{
    console.error('usage: stack-margin.ts [--guard <signature-regex>] [--max <bytes>] <signature-regex> <file-or-dir>...')
    process.exit(2)
}

const filter = new RegExp(pattern)
const guard = guardPattern === null ? null : new RegExp(guardPattern)
const files = new Map<string, Map<string, Node>>()
const defs = new Map<string, Node[]>()

for(const path of collect(roots))
{
    const nodes = parse(path)
    files.set(path, nodes)

    for(const n of nodes.values())
    {
        if(!n.defined) continue
        const key = bareName(n.title)
        const list = defs.get(key)
        if(list) list.push(n)
        else defs.set(key, [n])
    }
}

type Result = { bytes: number, path: Node[] }

let problems: string[] = []
let cuts: string[] = []
const memo = new Map<string, Result>()

function fail(what: string, from: Node, loc: string)
{
    const msg = `${what} (from ${from.sig} at ${loc})`
    if(!problems.includes(msg)) problems.push(msg)
}

function resolve(from: Node, edge: Edge): Node[]
{
    const local = files.get(from.file)?.get(edge.target)
    if(local?.defined) return [local]

    let found: Node[] | undefined
    for(const cand of aliases(bareName(edge.target)))
    {
        found = defs.get(cand)
        if(found) break
    }

    if(!found)
    {
        fail(`unresolved call to ${bareName(edge.target)}`, from, edge.loc)
        return []
    }

    return found
}

function best(nodes: Node[], stack: Node[]): Result
{
    let r: Result = { bytes: 0, path: [] }
    for(const n of nodes)
    {
        const c = walk(n, stack)
        if(c.bytes > r.bytes) r = c
    }
    return r
}

function walk(node: Node, stack: Node[]): Result
{
    /* A guarded callee re-checks the floor, so nothing below it belongs to
     * this margin — but its own prologue runs before that check, so its frame
     * does. Same rule for the root, which is why the root's frame is counted. */
    if(stack.length > 0 && guard !== null && guard.test(node.sig))
    {
        const msg = `${node.sig} (guarded, frame only, below ${stack[stack.length - 1].sig})`
        if(!cuts.includes(msg)) cuts.push(msg)
        return { bytes: node.frame, path: [node] }
    }

    if(stack.some(s => s.sig === node.sig))
    {
        fail(`unguarded recursion through ${node.sig}`, stack[stack.length - 1], node.loc)
        return { bytes: 0, path: [] }
    }

    const key = `${node.file}|${node.title}`
    const hit = memo.get(key)
    if(hit) return hit

    if(node.dynamicFrame) fail(`dynamic frame in ${node.sig}`, node, node.loc)
    if(node.dynamicObjects > 0) fail(`${node.dynamicObjects} dynamic object(s) in ${node.sig}`, node, node.loc)

    const cutsBefore = cuts.length
    let below: Result = { bytes: 0, path: [] }

    stack.push(node)
    for(const e of node.edges)
    {
        if(e.target === INDIRECT)
        {
            fail('indirect call', node, e.loc)
            continue
        }

        const c = best(resolve(node, e), stack)
        if(c.bytes > below.bytes) below = c
    }
    stack.pop()

    const r: Result = { bytes: node.frame + below.bytes, path: [node, ...below.path] }
    if(cuts.length === cutsBefore) memo.set(key, r)
    return r
}

const matches = new Map<string, Node[]>()

for(const nodes of files.values())
{
    for(const n of nodes.values())
    {
        if(!n.defined || !filter.test(n.sig)) continue
        const list = matches.get(n.sig)
        if(list) list.push(n)
        else matches.set(n.sig, [n])
    }
}

if(matches.size === 0)
{
    console.error(`no defined function matches /${pattern}/`)
    process.exit(2)
}

let failed = false

for(const sig of [...matches.keys()].sort())
{
    problems = []
    cuts = []
    memo.clear()

    const r = best(matches.get(sig)!, [])

    if(problems.length)
    {
        failed = true
        console.log(`${sig}: REJECTED`)
        for(const p of problems) console.log(`    ${p}`)
    }
    else
    {
        const over = r.bytes > max
        failed = failed || over
        console.log(`${sig}: ${r.bytes}${over ? ` — EXCEEDS --max ${max}` : ''}`)
        r.path.forEach((n, i) =>
        {
            const note = i > 0 && guard !== null && guard.test(n.sig) ? '   [guarded: frame only]' : ''
            console.log(`    ${String(n.frame).padStart(5)}  ${n.sig}${note}`)
        })
    }

    for(const c of cuts) console.log(`    cut: ${c}`)
}

process.exit(failed ? 1 : 0)
