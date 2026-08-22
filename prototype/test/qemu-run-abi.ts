/**
 * @ppl/jit-armv6m-prototype/test — real-ABI QEMU execution harness
 *
 * qemu-run.ts's counterpart for the real dispatch/eviction runtime
 * (docs/design.md), built once in
 * qemu/runtime_host.cpp + qemu/runtime.S rather than regenerated per test.
 * `callHelper`/`returnHelper`/the translator trampoline are all part of
 * that fixed runtime (qemu/runtime.S) — this harness's own generated
 * `program.cpp` only ever embeds what's actually per-program: every
 * procedure's own `[stub][body]` blob (programAbi.ts), and `arenaSize` as
 * a deliberately controllable knob — a generous value exercises the ABI
 * alone (test/abi-dispatch.test.ts); an undersized one forces real
 * eviction+compaction (test/eviction.test.ts).
 *
 * Three entry points into `enter_program`'s family (runtime_host.h),
 * sharing everything but the `main()` body each one needs:
 * `runAbiOnQemu` (the plain, static-global-arena original),
 * `runAbiOnStack` (the whole work area lives on the C stack —
 * test/enter-program-variants.test.ts), and `runAbiSplit` (the code
 * arena lives in a separate, caller-owned buffer, everything else still
 * on the C stack — same test file).
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

function procDecls(procs: readonly AbiCompiledProc[]): string
{
    return procs
        .map((p, i) => `__attribute__((section(".text.jitcode"))) static const uint16_t proc${i}[] = { ${toCArray(p.code)} };`)
        .join("\n")
}

function procTable(procs: readonly AbiCompiledProc[]): string
{
    return procs.map((_, i) => `{ proc${i}, sizeof(proc${i}) }`).join(", ")
}

const PROLOGUE = `
#include <stdint.h>
#include "runtime_host.h"

extern "C" {
void write_hex_result(uint32_t v);
void write_hex_trap(uint32_t v);
void semihosting_exit(int code);
}

// The linker's own marker for where .bss ends (../../src/linker.ld) — the
// one genuinely safe, realistic floor for a stackLimit that's actually
// dereferenced (enter_program_on_stack anchors its code arena there,
// runtime_host.cpp's own doc comment on why): RAM here is a real,
// non-negotiable 4096 bytes (linker.ld), so a plain "current sp minus a
// generous-looking constant" can walk straight past the bottom of RAM
// without ever touching a real address — harmless when stackLimit is
// only ever compared against, not written through, but not once this
// variant writes compiled code there.
extern uint8_t __bss_end;
`

const EPILOGUE = `
    if(r.trapped) write_hex_trap(r.value);
    else write_hex_result(r.value);
    semihosting_exit(0);
    return 0;
}
`

export interface AbiRunResult
{
    /** `false`: the entry procedure returned normally, `value` is its
     *  result. `true`: `RESOURCE_ERROR` (the arena emptied and the
     *  procedure being compiled still didn't fit, or — for the
     *  `*_on_stack`/`_split` variants — the stack-usage check itself
     *  failed before anything ran) or a bytecode `TRAP` that propagated
     *  all the way out — `value` is runtime_host.cpp's trap code. */
    readonly trapped: boolean
    readonly value: number
}

/** Writes `program.cpp`, builds `run.elf`, runs it under QEMU, and parses
 *  back the `RESULT:`/`TRAP:` line every variant's generated `main()`
 *  ultimately prints — the one piece all three entry points share. */
function buildAndRun(programC: string): AbiRunResult
{
    writeFileSync(path.join(QEMU_DIR, "program.cpp"), programC)
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

    throw new Error(`buildAndRun: no RESULT:/TRAP: line in QEMU output:\n${output}`)
}

/** Build and run a whole ABI-real-compiled program under QEMU. `arenaSize`
 *  is the deliberately-controllable work-area size (runtime_host.cpp) —
 *  generous to test the ABI alone, undersized to force eviction+compaction.
 *  No separate control-stack size knob anymore: call/return records live
 *  on the ordinary operand stack now (runtime_host.h's own header), so
 *  there's nothing left to size independently of `arenaSize`. */
export function runAbiOnQemu(procs: readonly AbiCompiledProc[], arenaSize: number, argIn = 0): AbiRunResult
{
    const programC = `${PROLOGUE}
${procDecls(procs)}

static const FlashProc procs[] = { ${procTable(procs)} };

int main(void)
{
    ProgramResult r = enter_program(${argIn | 0}, ${arenaSize}, procs, ${procs.length});
${EPILOGUE}`
    return buildAndRun(programC)
}

/** Options shared by `runAbiOnStack`/`runAbiSplit` — everything
 *  `requiredStackBytes` (runtime_host.cpp) needs beyond what it already
 *  knows about `procCount` on its own. `operandStackBytes`/`maxCallDepth`
 *  are the real, whole-program static properties runtime_host.h's own
 *  doc comments describe deriving from `validateProgram`'s `totalDepth` —
 *  callers below pass the genuine figures for their own test program, not
 *  placeholders. `slack` controls the one thing these tests need to
 *  vary: a small, non-negative margin *above* `__bss_end` (the one
 *  genuinely safe floor on this 4096-byte-RAM target — PROLOGUE's own
 *  comment) proves the happy path actually runs; `"reject"` forces
 *  `stackLimit` to sit at the generated `main()`'s own entry `sp` (zero
 *  headroom at all), so the check *must* fail before either variant ever
 *  touches `Runtime` or calls into `enter_dispatch`. */
export interface StackVariantOptions
{
    readonly operandStackBytes: number
    readonly maxCallDepth: number
    readonly interruptReserve?: number
    readonly slack: number | "reject"
}

function stackLimitExpr(slack: number | "reject"): string
{
    return slack === "reject" ? "entrySp" : `(uint32_t)(uintptr_t)&__bss_end + (${slack})`
}

/** `enter_program_on_stack` (runtime_host.h): the whole work area —
 *  `Runtime`, its dispatch table, the operand stack, and the compiled-
 *  code arena (`codeArenaSize`) — lives on the current C stack. */
export function runAbiOnStack(
    procs: readonly AbiCompiledProc[], codeArenaSize: number,
    opts: StackVariantOptions, argIn = 0,
): AbiRunResult
{
    const interruptReserve = opts.interruptReserve ?? 32
    const programC = `${PROLOGUE}
${procDecls(procs)}

static const FlashProc procs[] = { ${procTable(procs)} };

int main(void)
{
    register uint32_t entrySp asm("sp");
    uint32_t stackLimit = ${stackLimitExpr(opts.slack)};
    ProgramResult r = enter_program_on_stack(${argIn | 0}, procs, ${procs.length}, ${codeArenaSize},
        ${opts.operandStackBytes}, ${opts.maxCallDepth}, stackLimit, ${interruptReserve});
${EPILOGUE}`
    return buildAndRun(programC)
}

/** `enter_program_split` (runtime_host.h): the compiled-code arena lives
 *  in its own, separate buffer (standing in for a distinct SRAM bank/CCM
 *  a real target might use) — `Runtime`, its dispatch table, and the
 *  operand stack still live on the current C stack. */
export function runAbiSplit(
    procs: readonly AbiCompiledProc[], codeArenaSize: number,
    opts: StackVariantOptions, argIn = 0,
): AbiRunResult
{
    const interruptReserve = opts.interruptReserve ?? 32
    const programC = `${PROLOGUE}
${procDecls(procs)}

static const FlashProc procs[] = { ${procTable(procs)} };
__attribute__((section(".bss.codearena"))) static uint8_t codeArena[${codeArenaSize}];

int main(void)
{
    register uint32_t entrySp asm("sp");
    uint32_t stackLimit = ${stackLimitExpr(opts.slack)};
    ProgramResult r = enter_program_split(${argIn | 0}, procs, ${procs.length}, (uint32_t)(uintptr_t)codeArena, ${codeArenaSize},
        ${opts.operandStackBytes}, ${opts.maxCallDepth}, stackLimit, ${interruptReserve});
${EPILOGUE}`
    return buildAndRun(programC)
}
