/* fuzz — the target's coverage bitmap, read back against the image it came
 * from.
 *
 * `cov_rt.cpp` sets one bit per four bytes of rom, so a bit belongs to
 * exactly one basic block and this can invert it: how many blocks the image
 * has at all, which of them a run reached, and where the rest are in the
 * source. Nothing here mirrors a constant the target chose — the bitmap's
 * size and every block's address are read out of the ELF — so an image that
 * moves is followed rather than misreported.
 */
import * as crypto from "crypto"
import * as fs from "fs"
import { spawnSync } from "child_process"

const OBJDUMP = "arm-none-eabi-objdump"
const NM = "arm-none-eabi-nm"
const CPPFILT = "arm-none-eabi-c++filt"
const ADDR2LINE = "arm-none-eabi-addr2line"

export interface CovSite { addr: number; func: string; bucket: number }

/** Size of the target's bitmap, straight out of its symbol table. */
export function covBitmapBytes(elf: string): number | null
{
    const r = spawnSync(NM, ["-S", "--defined-only", elf], {encoding: "utf8"})
    if(r.status !== 0) return null
    const m = /^\S+ (\S+) . g_covBitmap$/m.exec(r.stdout)
    return m === null ? null : parseInt(m[1]!, 16)
}

export interface CovMap
{
    elf: string
    sites: readonly CovSite[]
    /** Every block's bit. One block per bit, so this is `sites.length`
     *  unless the target's index and this one have drifted apart. */
    buckets: ReadonlySet<number>
    /** Bytes of `g_covBitmap` in the image, which is how big a union buffer
     *  has to be for the target's `COV:` line to fit. */
    bitmapBytes: number
}

/** `cov_rt.cpp`'s index: the return address of the block's opening 4-byte
 *  `bl`, over four. Whether the Thumb bit is set does not matter — a `bl`
 *  sits on an even address, so `+ 4` and `+ 5` land in the same slot. */
const bucketOf = (blSite: number): number => (blSite + 4) >>> 2

/** Null when the toolchain is not on PATH: a campaign still runs, it just
 *  reports raw bits instead of a percentage of what is reachable. */
export function covMap(elf: string): CovMap | null
{
    const r = spawnSync(OBJDUMP, ["-d", elf], {encoding: "utf8", maxBuffer: 64 * 1024 * 1024})
    if(r.status !== 0) return null

    const sites: CovSite[] = []
    let func = "?"
    for(const line of r.stdout.split("\n"))
    {
        const head = /^[0-9a-f]{8} <(.+)>:/.exec(line)
        if(head !== null) { func = head[1]!; continue }

        const site = /^\s*([0-9a-f]+):\s.*\sbl\s+.*<__sanitizer_cov_trace_pc>/.exec(line)
        if(site !== null)
        {
            const addr = parseInt(site[1]!, 16)
            sites.push({addr, func, bucket: bucketOf(addr)})
        }
    }
    if(sites.length === 0) return null

    return {elf, sites, buckets: new Set(sites.map(s => s.bucket)), bitmapBytes: covBitmapBytes(elf) ?? 0}
}

export interface DarkFunction { func: string; dark: number; total: number; lines: string[] }

/** Functions holding blocks whose bucket is clear, worst first, each with
 *  the source lines those blocks start at — a name alone says a branch was
 *  missed, a line says which one. Demangled and resolved if the toolchain is
 *  there, mangled and lineless if it is not. */
export function darkFunctions(map: CovMap, bits: Uint8Array): DarkFunction[]
{
    const set = (b: number): boolean => ((bits[b >> 3] ?? 0) >> (b & 7) & 1) === 1

    const dark = new Map<string, {dark: number; total: number; addrs: number[]}>()
    for(const s of map.sites)
    {
        const e = dark.get(s.func) ?? {dark: 0, total: 0, addrs: []}
        e.total++
        if(!set(s.bucket)) { e.dark++; e.addrs.push(s.addr) }
        dark.set(s.func, e)
    }

    const rows = [...dark].filter(([, v]) => v.dark > 0)
        .map(([func, v]) => ({func, dark: v.dark, total: v.total, addrs: v.addrs, lines: [] as string[]}))
        .sort((a, b) => b.dark - a.dark || a.func.localeCompare(b.func))

    const flat = rows.flatMap(r => r.addrs)
    const where = spawnSync(ADDR2LINE, ["-e", map.elf, ...flat.map(a => a.toString(16))], {encoding: "utf8"})
    if(where.status === 0)
    {
        const at = where.stdout.split("\n").map(l => l.trim().replace(/^.*\//, ""))
        let i = 0
        for(const r of rows) r.lines = [...new Set(r.addrs.map(() => at[i++] ?? "?"))].filter(l => !l.startsWith("??") && !l.endsWith(":?"))
    }

    const filt = spawnSync(CPPFILT, [], {encoding: "utf8", input: rows.map(r => r.func).join("\n")})
    const names = filt.status === 0 ? filt.stdout.split("\n") : []
    // A demangled C++ name carries its whole signature; the campaign wants
    // to know which function, not which overload.
    return rows.map(({addrs, ...r}, i) => ({...r, func: (names[i] ?? r.func).replace(/\(.*$/, "")}))
}

/* A saved bitmap belongs to the image it was taken from — block addresses
 * move when anything in the image does, and a bitmap read against a
 * different build names the wrong blocks without looking wrong. */

export function covStamp(elf: string): string
{
    return crypto.createHash("sha256").update(fs.readFileSync(elf)).digest("hex").slice(0, 16)
}

export function saveCoverage(file: string, elf: string, bits: Uint8Array): void
{
    fs.writeFileSync(file, Buffer.from(bits))
    fs.writeFileSync(`${file}.elf`, covStamp(elf))
}

/** Null when the file is missing, and an error when it belongs to another
 *  build — never a silently wrong answer. */
export function loadCoverage(file: string, elf: string): Uint8Array | null
{
    if(!fs.existsSync(file)) return null

    const stamp = fs.existsSync(`${file}.elf`) ? fs.readFileSync(`${file}.elf`, "utf8").trim() : ""
    if(stamp !== covStamp(elf))
        throw new Error(`${file} was taken from a different build of ${elf} — re-run ./fuzz/fuzz.sh`)

    return new Uint8Array(fs.readFileSync(file))
}
