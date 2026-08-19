/**
 * @ppl/jit-armv6m-prototype/test — real-ABI QEMU execution harness
 *
 * qemu-run.ts's counterpart for the real dispatch/control-stack/eviction
 * runtime (docs/jit-armv6m-dispatch-handoff.html), built once in
 * qemu/runtime_host.c + qemu/trampoline.S rather than regenerated per test.
 * This harness's own generated `program.c` embeds every procedure's own
 * `[stub][body]` blob (programAbi.ts) as flash-resident data, plus
 * `callHelper`/`returnHelper` (runtime.ts), and calls `enter_program` with
 * `arenaSize` as a deliberately controllable knob — a generous value
 * exercises the ABI alone (test/abi-dispatch.test.ts); an undersized one
 * forces real eviction+compaction (test/eviction.test.ts).
 *
 * `enter_program` never returns in the ordinary C sense (runtime_host.c's
 * `landing_point` reports and halts internally), so unlike qemu-run.ts's
 * `main`, this harness's own generated `main` has nothing to do after the
 * call — the reported line is what this function parses back out.
 */

import { execFileSync, spawnSync } from "node:child_process"
import { writeFileSync } from "node:fs"
import path from "node:path"

import { emitCallHelper, emitReturnHelper } from "../src/runtime"
import type { AbiCompiledProc } from "../src/programAbi"

const QEMU_DIR = path.join(__dirname, "..", "qemu")

function toCArray(code: Uint16Array | readonly number[]): string
{
    return Array.from(code, w => `0x${(w & 0xffff).toString(16).padStart(4, "0")}`).join(", ")
}

function generateProgramC(procs: readonly AbiCompiledProc[], arenaSize: number, argIn: number): string
{
    const procDecls = procs
        .map((p, i) => `__attribute__((section(".text.jitcode"))) static const uint16_t proc${i}[] = { ${toCArray(p.code)} };`)
        .join("\n")
    const procTable = procs.map((_, i) => `{ proc${i}, sizeof(proc${i}) }`).join(", ")

    return `
#include <stdint.h>
#include "runtime_host.h"

__attribute__((section(".text.jitcode"))) static const uint16_t callHelperBlob[] = { ${toCArray(emitCallHelper())} };
__attribute__((section(".text.jitcode"))) static const uint16_t returnHelperBlob[] = { ${toCArray(emitReturnHelper())} };

${procDecls}

static const FlashProc procs[] = { ${procTable} };

int main(void)
{
    enter_program(${argIn | 0}, ${arenaSize}, procs, ${procs.length},
        ((uint32_t)(uintptr_t)callHelperBlob) | 1,
        ((uint32_t)(uintptr_t)returnHelperBlob) | 1);
    return 0;
}
`
}

export interface AbiRunResult
{
    /** `false`: the entry procedure returned normally, `value` is its
     *  result. `true`: `RESOURCE_ERROR` (the arena emptied and the
     *  procedure being compiled still didn't fit) or a bytecode `TRAP` that
     *  propagated all the way out — `value` is runtime_host.c's trap code. */
    readonly trapped: boolean
    readonly value: number
}

/** Build and run a whole ABI-real-compiled program under QEMU. `arenaSize`
 *  is the deliberately-controllable work-area size (runtime_host.c) —
 *  generous to test the ABI alone, undersized to force eviction+compaction. */
export function runAbiOnQemu(procs: readonly AbiCompiledProc[], arenaSize: number, argIn = 0): AbiRunResult
{
    writeFileSync(path.join(QEMU_DIR, "program.c"), generateProgramC(procs, arenaSize, argIn))
    execFileSync("make", ["-C", QEMU_DIR, "run.elf"], { stdio: "pipe" })

    const result = spawnSync("qemu-system-arm", [
        "-M", "lm3s811evb", "-m", "8k", "-nographic", "-monitor", "none",
        "-semihosting-config", "enable=on,target=native",
        "-kernel", path.join(QEMU_DIR, "run.elf"),
    ], { stdio: ["ignore", "pipe", "pipe"], timeout: 15_000 })
    if(result.error) throw result.error
    const output = result.stdout.toString("utf8") + result.stderr.toString("utf8")

    const resultMatch = output.match(/RESULT:([0-9a-f]{8})/)
    if(resultMatch) return { trapped: false, value: parseInt(resultMatch[1]!, 16) >>> 0 }

    const trapMatch = output.match(/TRAP:([0-9a-f]{8})/)
    if(trapMatch) return { trapped: true, value: parseInt(trapMatch[1]!, 16) >>> 0 }

    throw new Error(`runAbiOnQemu: no RESULT:/TRAP: line in QEMU output:\n${output}`)
}
