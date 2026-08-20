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

/* A stable C-ABI boundary on purpose: qemu/runtime.S reaches these by
 * plain, unmangled name (enter_dispatch/callHelper/returnHelper/
 * translator_trampoline, the other direction of this same boundary), and
 * a real host application linking this JIT in may just as easily be
 * plain C as C++ — extern "C" keeps both cases working, not just the
 * generated test program.cpp this prototype happens to use today. */
#ifdef __cplusplus
extern "C" {
#endif

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
 * integers), which this function unpacks into an ordinary by-value
 * `ProgramResult` return. That struct is 8 bytes — over AAPCS32's 4-byte
 * threshold for returning a composite in a register — so it still goes
 * out through a hidden pointer, but the compiler synthesizes that itself;
 * there's no hand-written asm on this side needing an explicit
 * out-parameter to write through.
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
ProgramResult enter_program(
    uint32_t argIn,
    uint32_t arenaSize,
    const FlashProc *procs,
    uint32_t procCount);

/** One ARMv6-M exception frame (R0-R3,R12,LR,return-address,xPSR — 32
 *  bytes, docs/jit-armv6m-dispatch-handoff.html §09's own interrupt-
 *  isolation figure). A convenience value for `interruptReserve` below,
 *  not the only legal one — a caller with its own RTOS/ISR requirements
 *  beyond the bare architectural minimum should pass whatever its own
 *  platform actually needs instead. */
#define ARMV6M_EXCEPTION_FRAME_BYTES 32

/**
 * `enter_program`, but the current C stack *is* the whole work area:
 * `Runtime`, its dispatch table, the operand stack, and the compiled-code
 * arena (sized to `codeArenaSize`) all come out of it, nothing left over
 * on any other region at all — but not stacked one after the other.
 * The arena anchors at `stackLimit` and grows *up* from there;
 * `Runtime`/the operand stack grow down from wherever `sp` already is,
 * converging toward each other from opposite ends of the same checked
 * range (runtime_host.cpp's own doc comment on this function has the
 * full reasoning — it leaves room for a future real translator to
 * temporarily encroach into whatever part of the arena's own reservation
 * is still unused, docs/jit-armv6m-dispatch-handoff.html §09's "translator's
 * own exception").
 *
 * Safe only because it's checked first — `operandStackBytes` (the
 * program's own worst-case TOS depth in bytes) and `maxCallDepth` (its
 * worst-case live call-chain length) are static, whole-program
 * properties the caller is expected to have already computed
 * (runtime_host.cpp's own `requiredStackBytes` doc comment explains the
 * robust way: `maxCallDepth` from `validateProgram`'s own call-graph DFS,
 * `operandStackBytes` as `totalDepth * 4` — the whole tight TOS-depth
 * bound, not a window-credited fraction of it, since the window's actual
 * absorption depends on call-boundary argument shuffling that abstract
 * depth alone doesn't capture); `stackLimit` is the lowest address this
 * excursion must never reach or pass, and `interruptReserve` is how much
 * headroom to hold back for exception entry
 * (`ARMV6M_EXCEPTION_FRAME_BYTES` covers the bare architectural minimum).
 * On failure, the returned `ProgramResult` reports RESOURCE_ERROR without
 * ever touching any of that memory or calling into `enter_dispatch` at all.
 */
ProgramResult enter_program_on_stack(
    uint32_t argIn,
    const FlashProc *procs,
    uint32_t procCount,
    uint32_t codeArenaSize,
    uint32_t operandStackBytes,
    uint32_t maxCallDepth,
    uint32_t stackLimit,
    uint32_t interruptReserve);

/**
 * `enter_program`, but for hardware where the compiled-code arena wants
 * to live somewhere other than plain stack-adjacent RAM (a distinct SRAM
 * bank, CCM, whatever a given target's bus matrix makes worth using) —
 * `codeArenaBase`/`codeArenaSize` describe that caller-owned region
 * directly. `Runtime`, its dispatch table, and the operand stack still
 * live on the current C stack regardless: the translator/helpers/
 * extensions calling into this are just ordinary C using that same
 * stack no matter where the arena itself sits, so there's no reason for
 * them to move too. Same stack-limit checking as `enter_program_on_stack`
 * (see its own doc comment for `operandStackBytes`/`maxCallDepth`/
 * `stackLimit`/`interruptReserve`) — except `codeArenaSize` never enters
 * that check at all, since that memory isn't on this stack to begin
 * with; sizing it correctly is the caller's own, separate concern.
 */
ProgramResult enter_program_split(
    uint32_t argIn,
    const FlashProc *procs,
    uint32_t procCount,
    uint32_t codeArenaBase,
    uint32_t codeArenaSize,
    uint32_t operandStackBytes,
    uint32_t maxCallDepth,
    uint32_t stackLimit,
    uint32_t interruptReserve);

#ifdef __cplusplus
}
#endif

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
