/* jit-armv6m/runtime — layer 2's own definitions: the fixed helper vector
 * built from runtime.S's own symbols, the trampoline address every
 * uncompiled dispatch slot points at, and the direct-exit escape an
 * attached Assembler (compiler/src/assembler.cpp's fail()) reaches when
 * it cannot free enough arena room. Nothing here is per-Runtime state —
 * every program execution shares these same three definitions.
 */
#include "dispatch_abi.h"

/* A plain fixed flash symbol, not per-Runtime state — every program
 * execution points every uncompiled slot at the same address. No `| 1u`
 * needed: .thumb_func (runtime.S) already bakes the Thumb bit into
 * translatorTrampoline's own symbol value. `extern` on the definition
 * itself, not just the declaration in runtime_internal.h: a const global
 * defaults to internal linkage in C++, unlike C. */
extern const uint32_t trampolineAddr = (uint32_t)(uintptr_t)translatorTrampoline;

/* r10 (helper vector base) — fixed for the whole program's lifetime, so
 * link-time const rather than something enterProgram fills in on every
 * call. No `| 1u` needed: .thumb_func (runtime.S) already bakes the Thumb
 * bit into each of these eight symbols' own value. Every RETURN/TRAP
 * dispatches to exactly one of indices 1/2/3/7 depending on savesLR and
 * initialSpilledCount (abi_strategy.cpp's abiEmitReturn) — index 3
 * (returnHelperTail) is also reached directly, by a plain branch, from
 * indices 1 and 7. Indices 4-6 (clzHelper/revbitsHelper/brTableJumpHelper)
 * are the reserved software-helper slots — see runtime.S's own header above
 * those three symbols. */
extern const uint32_t helperVec[8] = {
    (uint32_t)(uintptr_t)callHelper,
    (uint32_t)(uintptr_t)returnHelperFromLr,
    (uint32_t)(uintptr_t)returnHelperFromStack,
    (uint32_t)(uintptr_t)returnHelperTail,
    (uint32_t)(uintptr_t)clzHelper,
    (uint32_t)(uintptr_t)revbitsHelper,
    (uint32_t)(uintptr_t)brTableJumpHelper,
    (uint32_t)(uintptr_t)returnHelperFromStackReclaim,
};

/* An attached Assembler's own direct exit when it cannot free enough
 * arena room even after evicting everything resident
 * (compiler/src/assembler.cpp's fail()) — restores the caller's own saved
 * sp and transfers to the sentinel landing, tagged LANDING_TRAP,
 * unwinding the entire excursion including the trampoline's own pushed
 * frame. Never returns. Exactly parallel to enterDispatch's own
 * .Lresume/sentinel mechanism (runtime.S) — this is the same "get back to
 * the landing point from deep inside an excursion" convention, just
 * reached from C++ instead of from the callHelper/returnHelper* chain. */
extern "C" void runtimeBail(Runtime *runtime, uint32_t trapCode)
{
    register uint32_t trapCodeReg asm("r0") = trapCode;
    register uint32_t tagReg asm("r2") = LANDING_TRAP;
    register uint32_t landingReg asm("r3") = runtime->sentinelLandingAddress();
    register uint32_t savedSpReg asm("r1") = runtime->savedSp;
    asm volatile("mov sp, r1\n\tbx r3" : : "r"(trapCodeReg), "r"(savedSpReg), "r"(tagReg), "r"(landingReg));
    __builtin_unreachable();
}
