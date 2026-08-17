/**
 * @ppl/jit-armv6m-prototype/test — QEMU execution harness
 *
 * Generates a tiny C "trampoline" embedding one translated procedure's
 * native code as a `.text`-placed blob, builds it against the real
 * jit-armv6m vector table/linker script with the real `arm-none-eabi-gcc`
 * toolchain, and runs it under real `qemu-system-arm` — the whole point of
 * this session's prototype being a *native* translator rather than a
 * bytecode interpreter is that this is a genuine test of the generated
 * machine code, not a re-implementation of ARM semantics in JS.
 *
 * ABI (this prototype's own choice, matching docs/jit-armv6m.md §6/§7
 * exactly since nothing here disagrees with it): the argument (if any)
 * arrives in r0 (acc — §3) and the result leaves in r0 too, the same
 * register both ways, matching AAPCS's own argument/return convention.
 * The result travels back to this
 * process via a tagged semihosting `SYS_WRITE0` line, not the process exit
 * code — POSIX exit codes are truncated to 8 bits, nowhere near enough for
 * an arbitrary 32-bit `acc`. `qemu/Makefile` builds `program.c`
 * (regenerated here every call) against `semihosting.c` and the shared
 * `jit-armv6m/src/vectors.S`/`linker.ld`.
 */

import { execFileSync, spawnSync } from "node:child_process"
import { writeFileSync } from "node:fs"
import path from "node:path"

const QEMU_DIR = path.join(__dirname, "..", "qemu")

function toCArray(code: Uint16Array): string
{
    return Array.from(code, w => `0x${w.toString(16).padStart(4, "0")}`).join(", ")
}

function generateProgramC(code: Uint16Array, arg: number): string
{
    return `
#include <stdint.h>

extern void semihosting_exit(int code);
extern void write_hex_result(uint32_t v);

__attribute__((section(".text.jitcode")))
static const uint16_t code[] = { ${toCArray(code)} };

int main(void)
{
    register unsigned int result;
    asm volatile(
        "mov r0, %1\\n"
        "blx %2\\n"
        "mov %0, r0\\n"
        : "=r"(result)
        : "r"(${arg | 0}), "r"((uint32_t)((uintptr_t)code | 1))
        : "r0", "r3", "lr", "cc"
    );
    write_hex_result(result);
    semihosting_exit(0);
    return 0;
}
`
}

/**
 * Build and run one translated procedure under QEMU. Returns the value
 * left in r0 at `RETURN` — or, per translateProc.ts's own sentinel
 * (`0x80000000 | trapCode`), throws if the procedure `TRAP`ped instead of
 * returning normally.
 */
export function runOnQemu(code: Uint16Array, arg = 0): number
{
    // No `make clean` — program.c's refreshed mtime is enough for make's
    // own incremental rebuild to relink; vectors.S.o/semihosting.c.o stay
    // cached across calls, which matters since this runs once per test case.
    writeFileSync(path.join(QEMU_DIR, "program.c"), generateProgramC(code, arg))
    execFileSync("make", ["-C", QEMU_DIR, "run.elf"], { stdio: "pipe" })

    // QEMU's `-nographic` console (and hence the guest's semihosting
    // SYS_WRITE0 output) lands on *stderr*, not stdout — easy to miss
    // since a real terminal shows both interleaved. `-monitor none`
    // decouples the QEMU monitor from stdio so an immediately-closed
    // stdin (as under a non-interactive child process) doesn't read as
    // "EOF on the monitor" and trigger a silent, instant quit.
    const result = spawnSync("qemu-system-arm", [
        "-M", "lm3s811evb", "-m", "8k", "-nographic", "-monitor", "none",
        "-semihosting-config", "enable=on,target=native",
        "-kernel", path.join(QEMU_DIR, "run.elf"),
    ], { stdio: ["ignore", "pipe", "pipe"], timeout: 15_000 })
    if(result.error) throw result.error
    const output = result.stdout.toString("utf8") + result.stderr.toString("utf8")

    const match = output.match(/RESULT:([0-9a-f]{8})/)
    if(!match) throw new Error(`runOnQemu: no RESULT: line in QEMU output:\n${output}`)
    const value = parseInt(match[1]!, 16) >>> 0
    if(value & 0x80000000) throw new Error(`runOnQemu: procedure trapped, code ${value & 0x7fffffff}`)
    return value
}
