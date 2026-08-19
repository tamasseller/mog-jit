/**
 * @ppl/jit-armv6m-prototype/test — real-ABI QEMU execution harness
 *
 * qemu-run.ts's counterpart for the real dispatch/eviction runtime
 * (docs/jit-armv6m-dispatch-handoff.html), built once in
 * qemu/runtime_host.c + qemu/runtime.S rather than regenerated per test.
 * `callHelper`/`returnHelper`/the translator trampoline are all part of
 * that fixed runtime (qemu/runtime.S) — this harness's own generated
 * `program.c` only ever embeds what's actually per-program: every
 * procedure's own `[stub][body]` blob (programAbi.ts), and `arenaSize` as
 * a deliberately controllable knob — a generous value exercises the ABI
 * alone (test/abi-dispatch.test.ts); an undersized one forces real
 * eviction+compaction (test/eviction.test.ts).
 */

import { execFileSync, spawnSync } from "node:child_process"
import { writeFileSync } from "node:fs"
import path from "node:path"

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

extern void write_hex_result(uint32_t v);
extern void write_hex_trap(uint32_t v);
extern void semihosting_exit(int code);

${procDecls}

static const FlashProc procs[] = { ${procTable} };

int main(void)
{
    ProgramResult r;
    enter_program(${argIn | 0}, ${arenaSize}, procs, ${procs.length}, &r);
    if(r.trapped) write_hex_trap(r.value);
    else write_hex_result(r.value);
    semihosting_exit(0);
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
 *  generous to test the ABI alone, undersized to force eviction+compaction.
 *  No separate control-stack size knob anymore: call/return records live
 *  on the ordinary operand stack now (runtime_host.h's own header), so
 *  there's nothing left to size independently of `arenaSize`. */
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
