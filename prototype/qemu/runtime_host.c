/* enter_program + the mock translator (docs/jit-armv6m-dispatch-handoff.html
 * §09) — a real dispatch table and info block, call/return records living
 * on the ordinary operand stack rather than a separate control stack, and
 * eviction+compaction driven by a deliberately small `arenaSize`, but
 * "compilation" itself replaced by a memcpy from an already-ABI-compliant
 * flash blob (test/qemu-run-abi.ts). Exercises the runtime machinery a real
 * translator would rely on, without needing the real translator.
 *
 * Built with -ffixed-r8/r9/r10/r11 (qemu/Makefile) — those four registers
 * carry live JIT state (dispatch table base, runtime pointer, helper
 * vector base, LRU tick) across `compileProc`'s own call into this file, so
 * removing them from the compiler's register allocator entirely is a
 * stronger, toolchain-enforced guarantee than leaning on ordinary
 * callee-saved convention (the open question this session flagged about r9
 * specifically — docs/jit-armv6m.md's own §2 addendum).
 */

#include <stdint.h>
#include <string.h>
#include <stddef.h>
#include "runtime_host.h"

#define ARENA_CAPACITY 512

#define LANDING_TRAP 1u /* the boot record's tag half is 0 — enter_dispatch's own literal, qemu/runtime.S */
#define RESOURCE_ERROR_CODE 0x52455343u /* "RESC", arbitrary/distinct */

typedef struct { uint32_t code_ptr; uint32_t last_used; } DispatchEntry;

/* The whole per-program-execution state, one object, allocated once by
 * enter_program on its own stack frame (no more file-scope statics) and
 * reached everywhere else — compileProc, bailOut, enter_dispatch — via a
 * single pointer (r9) rather than a global. `dispatchTable` is a C99
 * flexible array member, indexed in its own, natural, 0-based terms:
 * `[0]` is the sentinel (docs/jit-armv6m-dispatch-handoff.html §09's
 * info-block idea, made literal here), `[1..procCount]` are the real
 * per-procedure slots. The ABI's own dispatchBase (r8) is `&dispatchTable[1]`
 * — one past the sentinel — so the asm side's 0-based procedure index
 * `i` is always this struct's own `dispatchTable[i + 1]`; nothing here
 * indexes `dispatchTable` with a bare asm-style `idx` unadjusted. One
 * FAM, not two: the control stack used to need its own array too, but
 * it's folded into the ordinary operand stack now (runtime_host.h's own
 * header), so there's nothing left needing a second variable-length
 * member — the kind of struct C99's flexible-array-member idiom is built
 * for, no GNU extension or type-punning tradeoff required. */
typedef struct
{
    uint32_t savedSp;
    uint32_t arenaBase;
    uint32_t arenaEnd;
    uint32_t arenaCursor;
    const FlashProc *flashProcs;
    uint32_t procCount;
    DispatchEntry dispatchTable[];
} Runtime;

/* qemu/runtime.S hardcodes both of these (runtime_host.h) since it can't
 * call `offsetof` itself — these ties catch any layout drift at compile
 * time instead of letting it corrupt memory silently. */
_Static_assert(offsetof(Runtime, dispatchTable) + sizeof(DispatchEntry) == RUNTIME_DISPATCH_TABLE_OFFSET,
    "runtime.S's own RUNTIME_DISPATCH_TABLE_OFFSET must match Runtime's real layout");
_Static_assert(sizeof(DispatchEntry) == DISPATCH_SENTINEL_OFFSET,
    "runtime.S's own DISPATCH_SENTINEL_OFFSET must match sizeof(DispatchEntry)");

static uint8_t g_arenaStorage[ARENA_CAPACITY];

extern void translator_trampoline(void); /* qemu/runtime.S */
extern const uint16_t callHelper[];       /* qemu/runtime.S */
extern const uint16_t returnHelper[];     /* qemu/runtime.S */
extern uint64_t enter_dispatch(uint32_t argIn, Runtime *runtime); /* qemu/runtime.S */

/* A plain fixed flash symbol, not per-Runtime state — every program
 * execution points every uncompiled slot at the same address, so there's
 * nothing to gain by carrying a copy of it inside `Runtime`. No `| 1u`
 * needed, same reasoning as `g_helperVec` just below: `.thumb_func`
 * (qemu/runtime.S) already bakes the Thumb bit into translator_trampoline's
 * own symbol value. */
static const uint32_t g_trampolineAddr = (uint32_t)(uintptr_t)translator_trampoline;

/* r10 (helper vector base) — fixed for the whole program's lifetime, so
 * link-time const rather than something enter_program fills in on every
 * call. No `| 1u`/`+ 1u` needed: `.thumb_func` (qemu/runtime.S) already
 * bakes the Thumb bit into callHelper/returnHelper's own symbol value
 * (confirmed via readelf — their st_value is already odd), and the plain
 * R_ARM_ABS32 relocation this cast produces resolves against that value
 * directly. */
const uint32_t g_helperVec[2] = {
    (uint32_t)(uintptr_t)callHelper,
    (uint32_t)(uintptr_t)returnHelper,
};

/** Jumps straight to the same landing address a normal return would reach
 *  off the dispatch table's sentinel slot — but directly, bypassing
 *  returnHelper entirely, with LANDING_TRAP instead of whatever tag
 *  `enter_dispatch` (qemu/runtime.S) bootstrapped with (its own landing
 *  point is what actually disambiguates the two via r2). */
static void __attribute__((noreturn)) bailOut(Runtime *rt, uint32_t trapCode)
{
    register uint32_t r0_ asm("r0") = trapCode;
    register uint32_t r2_ asm("r2") = LANDING_TRAP;
    register uint32_t r3_ asm("r3") = rt->dispatchTable[0].code_ptr; /* the sentinel */
    register uint32_t r1_ asm("r1") = rt->savedSp;
    /* Restore sp before jumping away — see savedSp's own header. */
    asm volatile("mov sp, r1\n\tbx r3" : : "r"(r0_), "r"(r1_), "r"(r2_), "r"(r3_));
    __builtin_unreachable();
}

/** The mock translator: "compiling" procedure `idx` is a memcpy from its
 *  already-ABI-compliant flash blob, not real code generation — but the
 *  space accounting, eviction (global LRU minimum, nothing pinned — §04/
 *  §08), and compaction (memmove + fix up just the moved slots' own
 *  code_ptr, no other patching — §11's position-independence) are the real
 *  thing. Reached only from qemu/runtime.S, never called directly by
 *  translated code. */
void compileProc(uint32_t idx, Runtime *rt)
{
    const FlashProc *proc = &rt->flashProcs[idx];
    uint32_t need = proc->size;

    /* r11 (LRU_TICK_REG, src/registers.ts) — read, never written here:
     * -ffixed-r11 (qemu/Makefile) means nothing in this file ever touches
     * it, so it still holds whatever the prologue stub last bumped it to.
     * Needed below to compare `last_used` stamps in a way that survives
     * the counter wrapping, which a tight enough CALL/RETURN loop reaches
     * in seconds, not years. */
    register uint32_t now asm("r11");

    while(rt->arenaEnd - rt->arenaCursor < need)
    {
        int victim = -1;
        uint32_t oldestAge = 0;
        for(uint32_t i = 0; i < rt->procCount; i++)
        {
            DispatchEntry *e = &rt->dispatchTable[i + 1];
            if(e->code_ptr == g_trampolineAddr) continue; /* not resident */
            /* Age relative to `now`, not a raw magnitude compare on
             * `last_used` itself — unsigned, not signed: `last_used` was
             * always stamped at or before `now`, never after, so plain
             * modular subtraction already recovers the true elapsed tick
             * count exactly, for any true gap up to just under 2^32 (not
             * 2^31 — that halving is only needed when comparing two
             * sequence numbers with no shared "now" to anchor against,
             * which isn't the situation here). */
            uint32_t age = now - e->last_used;
            if(victim < 0 || age > oldestAge) { oldestAge = age; victim = (int)i; }
        }
        if(victim < 0) bailOut(rt, RESOURCE_ERROR_CODE); /* table empty, still doesn't fit */

        DispatchEntry *ve = &rt->dispatchTable[(uint32_t)victim + 1];
        uint32_t victimAddr = ve->code_ptr & ~1u;

        /* The victim's occupied size isn't a stored field (docs/jit-armv6m-
         * dispatch-handoff.html §03) — a real translator wouldn't have one
         * for an already-compiled *other* procedure, only for whatever it's
         * generating right now. Find it the same way compaction has to
         * anyway: scan for whichever other resident entry's code_ptr is the
         * next-closest one above this victim's, or the arena's current
         * high-water mark if nothing sits above it. */
        uint32_t gapEnd = rt->arenaCursor;
        for(uint32_t i = 0; i < rt->procCount; i++)
        {
            DispatchEntry *e = &rt->dispatchTable[i + 1];
            if(e->code_ptr == g_trampolineAddr) continue;
            uint32_t addr = e->code_ptr & ~1u;
            if(addr > victimAddr && addr < gapEnd) gapEnd = addr;
        }
        uint32_t victimSize = gapEnd - victimAddr;
        uint32_t tailLen = rt->arenaCursor - gapEnd;

        memmove((void *)(uintptr_t)victimAddr, (void *)(uintptr_t)gapEnd, tailLen);
        rt->arenaCursor -= victimSize;
        ve->code_ptr = g_trampolineAddr;
        ve->last_used = 0;

        for(uint32_t i = 0; i < rt->procCount; i++)
        {
            DispatchEntry *e = &rt->dispatchTable[i + 1];
            if(e->code_ptr != g_trampolineAddr && (e->code_ptr & ~1u) >= gapEnd)
                e->code_ptr -= victimSize;
        }
    }

    uint32_t dest = rt->arenaCursor;
    memcpy((void *)(uintptr_t)dest, proc->bytes, need);
    rt->arenaCursor += need;

    rt->dispatchTable[idx + 1].code_ptr = dest | 1u;
    rt->dispatchTable[idx + 1].last_used = 0; /* the freshly-copied prologue stub bumps this on entry */
}

void enter_program(
    uint32_t argIn, uint32_t arenaSize,
    const FlashProc *procs, uint32_t procCount,
    ProgramResult *outResult)
{
    /* One flexible-array-member object, over-allocated to fit procCount+1
     * dispatch entries (index 0 = sentinel) — sized and aligned by hand
     * since a plain `Runtime runtime;` local would only reserve the fixed
     * header (a FAM contributes 0 to sizeof), not the trailing slots. */
    _Alignas(Runtime) unsigned char runtimeStorage[sizeof(Runtime) + (procCount + 1) * sizeof(DispatchEntry)];
    Runtime *runtime = (Runtime *)runtimeStorage;

    runtime->arenaBase = (uint32_t)(uintptr_t)g_arenaStorage;
    runtime->arenaEnd = runtime->arenaBase + arenaSize;
    runtime->arenaCursor = runtime->arenaBase;
    runtime->flashProcs = procs;
    runtime->procCount = procCount;

    for(uint32_t i = 0; i < procCount; i++)
    {
        runtime->dispatchTable[i + 1].code_ptr = g_trampolineAddr;
        runtime->dispatchTable[i + 1].last_used = 0;
    }

    /* enter_dispatch (qemu/runtime.S) does the actual excursion — an
     * ordinary AAPCS call, so nothing here needs a clobber list or an
     * out-parameter. Its result comes back as a uint64_t register pair
     * (r0:r1), not a 2-word struct: AAPCS32 only returns composites that
     * small in registers up to 4 bytes, so a {value,trapped} struct would
     * take the hidden-pointer path instead — the same hazard the old
     * out-parameter design existed to avoid in the first place. */
    uint64_t packed = enter_dispatch(argIn, runtime);
    outResult->value = (uint32_t)packed;
    outResult->trapped = (uint32_t)(packed >> 32);
}
