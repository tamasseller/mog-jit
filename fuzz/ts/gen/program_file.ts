// fuzz — a finding's repro, on disk.
//
// `(corpus entry, seed)` identifies a program only while the corpus is the
// fixed seed set. A campaign feeds novel programs back into it, so by the
// time a finding appears the entry that produced it is usually a derived
// program several generations deep, and the pair names its distant
// ancestor instead. The program itself is therefore what gets saved.
//
// Saved as source, one entry per procedure, because that is the form a
// person reads and the printer round-trips it faithfully anyway.

import * as fs from "fs"
import { ir } from "mog-core"
import type { GenProgram } from "./corpus"
import { checkGraph } from "./corpus"
import { print } from "./print"

interface Saved
{
    procs: {args: string[]; source: string}[]
}

export function saveProgram(path: string, gen: GenProgram): void
{
    const saved: Saved = {procs: gen.procs.map(p => ({args: p.args, source: print(p.body)}))}
    fs.writeFileSync(path, JSON.stringify(saved, null, 2))
}

export function loadProgram(path: string): GenProgram
{
    const saved = JSON.parse(fs.readFileSync(path, "utf8")) as Saved
    const gen: GenProgram = {procs: saved.procs.map(p => ({args: p.args, body: [...ir`${p.source}`.body]}))}
    checkGraph(gen)
    return gen
}
