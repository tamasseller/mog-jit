// Compares the QEMU run's tagged output against the reference VM's results.
//
// Usage: npx ts-node --transpile-only bench/compare-check.ts <expected.json> <qemu-output>

import {readFileSync} from "node:fs"

interface Expected
{
    acc: number
    outputHash: number
    eventCount: number
    events: number[]
    samples: number
}

function tagged(text: string, prefix: string): number[]
{
    const out: number[] = []

    for(const line of text.split("\n"))
    {
        const t = line.trim()
        if(t.startsWith(prefix)) out.push(parseInt(t.slice(prefix.length), 16) >>> 0)
    }

    return out
}

function main(): void
{
    const [expectedPath, outputPath] = process.argv.slice(2)
    if(expectedPath === undefined || outputPath === undefined)
    {
        throw new Error("usage: compare-check.ts <expected.json> <qemu-output>")
    }

    const want: Expected = JSON.parse(readFileSync(expectedPath, "utf8"))
    const got = readFileSync(outputPath, "utf8")

    const fail = (msg: string): never =>
    {
        console.error(`FAIL: ${msg}`)
        console.error(`--- qemu output ---\n${got}`)
        process.exit(1)
    }

    /* A trap or a resource bail is a failure, not a skip: this program is
     * fixed, known to validate, and known to fit the arena. */
    const traps = tagged(got, "T:")
    if(traps.length > 0) fail(`the target trapped with code ${traps[0]}`)

    const bails = tagged(got, "E:")
    if(bails.length > 0) fail(`the target bailed with RESOURCE code 0x${bails[0]!.toString(16)}`)

    if(tagged(got, "DONE:").length === 0) fail("the target never reached DONE — it died mid-run")

    const [acc] = tagged(got, "R:")
    const [hash] = tagged(got, "H:")
    const [count] = tagged(got, "N:")
    const events = tagged(got, "V:")

    if(acc !== want.acc) fail(`return value: target ${acc}, reference ${want.acc}`)

    if(hash !== want.outputHash)
    {
        fail(`output stream hash: target 0x${hash?.toString(16)}, `
            + `reference 0x${want.outputHash.toString(16)}`)
    }

    if(count !== want.eventCount) fail(`trigger count: target ${count}, reference ${want.eventCount}`)

    if(events.length !== want.events.length)
    {
        fail(`event ring size: target reported ${events.length}, reference has ${want.events.length}`)
    }

    for(let i = 0; i < want.events.length; i++)
    {
        if(events[i] !== want.events[i])
        {
            fail(`event ring slot ${i}: target 0x${events[i]?.toString(16)}, `
                + `reference 0x${want.events[i]!.toString(16)}`)
        }
    }

    console.log(`PASS: ${want.samples} samples, ${want.eventCount} triggers, `
        + `return=${want.acc}, output hash=0x${want.outputHash.toString(16)} — `
        + `emitted Thumb agrees with the reference VM on every value`)
}

main()
