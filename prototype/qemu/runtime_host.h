/* Public interface into runtime_host.c's `enter_program` — the real-ABI
 * counterpart to test/qemu-run.ts's own bare "blx code" trampoline. Shared
 * between runtime_host.c and the per-test generated program.c
 * (test/qemu-run-abi.ts) so the `FlashProc` layout can't drift between the
 * two sides of that boundary. */

/* qemu/runtime.S includes this too (for the two #defines at the bottom),
 * preprocessed but never actually compiled as C — __ASSEMBLER__ is
 * predefined by GCC for that case specifically, so the struct/function
 * declarations below (meaningless, unparseable input to the assembler)
 * stay hidden from it. */
#ifndef __ASSEMBLER__

#include <stdint.h>

typedef struct
{
    const uint16_t *bytes; /* this procedure's own [stub][body] blob, in flash */
    uint32_t size;          /* its byte length */
} FlashProc;

typedef struct
{
    uint32_t value;   /* the entry procedure's own result, or (if trapped) a trap/error code */
    uint32_t trapped; /* 0: normal return. nonzero: RESOURCE_ERROR or a bytecode TRAP propagated out */
} ProgramResult;

/**
 * Sets up the dispatch table, then makes one ordinary call into
 * `enter_dispatch` (qemu/runtime.S), which dispatches into the entry
 * procedure (index 0) via the real `callHelper`/`returnHelper` ABI and
 * returns *normally* once that either completes or bails out. The raw
 * `BX`-chain control transfers in between carry no meaning to the C
 * compiler either way, but expressed as an ordinary hand-written function
 * rather than C inline asm, none of that needs a clobber list or a
 * register-starved operand list: `enter_dispatch` takes its inputs as
 * plain arguments and hands back a packed `uint64_t` (value in r0, trapped
 * in r1 — AAPCS32's own register-pair convention for double-word
 * integers), which this function unpacks into `*outResult` after an
 * ordinary return.
 *
 * `callHelper`/`returnHelper`/`enter_dispatch` themselves aren't
 * parameters — they're part of the fixed runtime (qemu/runtime.S,
 * alongside `translator_trampoline`), not something that varies per
 * program, so runtime_host.c just `extern`-declares them by name.
 *
 * There's no separate control stack anymore: `callHelper`/`returnHelper`
 * push/pop each call's own return record on the ordinary operand stack
 * (`sp`) now, the same one window spills already use — matching
 * docs/jit-armv6m-dispatch-handoff.html §06's own "sp is the real C stack,
 * not a second one" all the way, rather than stopping short of it for
 * call/return bookkeeping specifically. r9 is freed up by this (no more
 * control-stack pointer to track) and repurposed to hold the runtime
 * pointer directly.
 */
void enter_program(
    uint32_t argIn,
    uint32_t arenaSize,
    const FlashProc *procs,
    uint32_t procCount,
    ProgramResult *outResult);

#endif /* __ASSEMBLER__ */

/* qemu/runtime.S's own layout constants for reaching runtime_host.c's
 * (file-scope) `Runtime` struct from a bare address — `enter_dispatch`
 * gets the runtime pointer directly (r9), but still needs its dispatch
 * table's own base address (r8) and the sentinel slot one `DispatchEntry`
 * before it; runtime_host.c's own `_Static_assert`s tie these back to the
 * real struct, so any layout drift fails the build instead of corrupting
 * memory silently. */
#define RUNTIME_DISPATCH_TABLE_OFFSET 32 /* &runtime + this = dispatchBase (== &dispatchTable[1]) */
#define DISPATCH_SENTINEL_OFFSET 8       /* dispatchBase - this = &dispatchTable[0] (the sentinel) */
