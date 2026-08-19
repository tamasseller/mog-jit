/* Public interface into runtime_host.c's `enter_program` — the real-ABI
 * counterpart to test/qemu-run.ts's own bare "blx code" trampoline. Shared
 * between runtime_host.c and the per-test generated program.c
 * (test/qemu-run-abi.ts) so the `FlashProc` layout can't drift between the
 * two sides of that boundary. */

#include <stdint.h>

typedef struct
{
    const uint16_t *bytes; /* this procedure's own [stub][body] blob, in flash */
    uint32_t size;          /* its byte length */
} FlashProc;

/**
 * Sets up the dispatch table + control stack + info block, then tail-jumps
 * into the entry procedure (index 0) via the real `callHelper`/`returnHelper`
 * ABI. Never returns in the ordinary C sense — control eventually reaches
 * `landing_point` (runtime_host.c), which reports the result/trap via
 * semihosting and halts the CPU.
 *
 * `callHelperAddr`/`returnHelperAddr` must already have the Thumb-mode bit
 * set (the address of a `static const uint16_t[]` blob, `| 1`).
 */
void __attribute__((noreturn)) enter_program(
    uint32_t argIn,
    uint32_t arenaSize,
    const FlashProc *procs,
    uint32_t procCount,
    uint32_t callHelperAddr,
    uint32_t returnHelperAddr);
