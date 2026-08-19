/* enter_program + the mock translator (docs/jit-armv6m-dispatch-handoff.html
 * §09) — a real dispatch table, control stack, and info block, with
 * eviction+compaction driven by a deliberately small `arenaSize`, but
 * "compilation" itself replaced by a memcpy from an already-ABI-compliant
 * flash blob (test/qemu-run-abi.ts). Exercises the runtime machinery a real
 * translator would rely on, without needing the real translator.
 *
 * Built with -ffixed-r8/r9/r10/r11 (qemu/Makefile) — those four registers
 * carry live JIT state (dispatch table base, control stack pointer, helper
 * vector base, LRU tick) across `compileProc`'s own call into this file, so
 * removing them from the compiler's register allocator entirely is a
 * stronger, toolchain-enforced guarantee than leaning on ordinary
 * callee-saved convention (the open question this session flagged about r9
 * specifically — docs/jit-armv6m.md's own §2 addendum).
 */

#include <stdint.h>
#include <string.h>
#include "runtime_host.h"

#define MAX_PROCS 16
#define ARENA_CAPACITY 512
#define CTRL_STACK_CAPACITY 64

#define LANDING_SUCCESS 0u
#define LANDING_TRAP 1u
#define RESOURCE_ERROR_CODE 0x52455343u /* "RESC", arbitrary/distinct */

typedef struct { uint32_t code_ptr; uint32_t last_used; } DispatchEntry;

typedef struct
{
    uint32_t arenaBase;
    uint32_t arenaEnd;
    uint32_t arenaCursor;
    const FlashProc *flashProcs;
    uint32_t procCount;
    uint32_t trampolineAddr; /* Thumb-bit set */

    /* Must immediately precede dispatchTable — this is dispatch-table
     * index -1, reached by returnHelper's ordinary idx*8 arithmetic once
     * the sentinel proc_idx (0xffff) sign-extends (src/runtime.ts). */
    DispatchEntry enterProgramReturn;
    DispatchEntry dispatchTable[MAX_PROCS];
} Runtime;

static Runtime g_runtime;
static uint8_t g_arenaStorage[ARENA_CAPACITY];
static uint32_t g_ctrlStack[CTRL_STACK_CAPACITY];
static uint32_t g_helperVec[2]; /* 0 = callHelper, 1 = returnHelper */

extern void translator_trampoline(void); /* qemu/trampoline.S */
extern void write_hex_result(uint32_t v);
extern void write_hex_trap(uint32_t v);
extern void semihosting_exit(int code);

#define PACK_RECORD(procIdx, offsetPlus1) \
    (((uint32_t)(uint16_t)(procIdx)) | ((uint32_t)(offsetPlus1) << 16))

/** Jumps straight to the same landing address a normal return would pop
 *  off the control stack's sentinel slot — but directly, bypassing
 *  returnHelper entirely, with LANDING_TRAP instead of whatever tag
 *  `enter_program` bootstrapped with. `landing_point` (below) is what
 *  actually disambiguates the two. */
static void __attribute__((noreturn)) bailOut(uint32_t trapCode)
{
    register uint32_t r0_ asm("r0") = trapCode;
    register uint32_t r2_ asm("r2") = LANDING_TRAP;
    register uint32_t r3_ asm("r3") = g_runtime.enterProgramReturn.code_ptr;
    asm volatile("bx r3" : : "r"(r0_), "r"(r2_), "r"(r3_));
    __builtin_unreachable();
}

/** The mock translator: "compiling" procedure `idx` is a memcpy from its
 *  already-ABI-compliant flash blob, not real code generation — but the
 *  space accounting, eviction (global LRU minimum, nothing pinned — §04/
 *  §08), and compaction (memmove + fix up just the moved slots' own
 *  code_ptr, no other patching — §11's position-independence) are the real
 *  thing. Reached only from qemu/trampoline.S, never called directly by
 *  translated code. */
void compileProc(uint32_t idx)
{
    const FlashProc *proc = &g_runtime.flashProcs[idx];
    uint32_t need = proc->size;

    while(g_runtime.arenaEnd - g_runtime.arenaCursor < need)
    {
        int victim = -1;
        uint32_t oldest = 0;
        for(uint32_t i = 0; i < g_runtime.procCount; i++)
        {
            DispatchEntry *e = &g_runtime.dispatchTable[i];
            if(e->code_ptr == g_runtime.trampolineAddr) continue; /* not resident */
            if(victim < 0 || e->last_used < oldest) { oldest = e->last_used; victim = (int)i; }
        }
        if(victim < 0) bailOut(RESOURCE_ERROR_CODE); /* table empty, still doesn't fit */

        DispatchEntry *ve = &g_runtime.dispatchTable[(uint32_t)victim];
        uint32_t victimAddr = ve->code_ptr & ~1u;
        uint32_t victimSize = g_runtime.flashProcs[(uint32_t)victim].size;
        uint32_t gapEnd = victimAddr + victimSize;
        uint32_t tailLen = g_runtime.arenaCursor - gapEnd;

        memmove((void *)(uintptr_t)victimAddr, (void *)(uintptr_t)gapEnd, tailLen);
        g_runtime.arenaCursor -= victimSize;
        ve->code_ptr = g_runtime.trampolineAddr;
        ve->last_used = 0;

        for(uint32_t i = 0; i < g_runtime.procCount; i++)
        {
            DispatchEntry *e = &g_runtime.dispatchTable[i];
            if(e->code_ptr != g_runtime.trampolineAddr && (e->code_ptr & ~1u) >= gapEnd)
                e->code_ptr -= victimSize;
        }
    }

    uint32_t dest = g_runtime.arenaCursor;
    memcpy((void *)(uintptr_t)dest, proc->bytes, need);
    g_runtime.arenaCursor += need;

    g_runtime.dispatchTable[idx].code_ptr = dest | 1u;
    g_runtime.dispatchTable[idx].last_used = 0; /* the freshly-copied prologue stub bumps this on entry */
}

/** Where the sentinel dispatch-table-entry-shaped slot (idx -1) always
 *  points — a normal `RETURN` out of the entry procedure, and `bailOut`
 *  above, both land here, disambiguated purely by r2 (this session's
 *  agreed convention: r2 carries whatever tag was written into the record
 *  that got dispatched here, no special-casing anywhere upstream). */
void landing_point(void)
{
    register uint32_t resultReg, tagReg;
    asm volatile("mov %0, r0\n\tmov %1, r2" : "=r"(resultReg), "=r"(tagReg));
    if(tagReg == LANDING_SUCCESS) write_hex_result(resultReg);
    else write_hex_trap(resultReg);
    semihosting_exit(0);
    __builtin_unreachable();
}

void __attribute__((noreturn)) enter_program(
    uint32_t argIn, uint32_t arenaSize,
    const FlashProc *procs, uint32_t procCount,
    uint32_t callHelperAddr, uint32_t returnHelperAddr)
{
    g_runtime.arenaBase = (uint32_t)(uintptr_t)g_arenaStorage;
    g_runtime.arenaEnd = g_runtime.arenaBase + arenaSize;
    g_runtime.arenaCursor = g_runtime.arenaBase;
    g_runtime.flashProcs = procs;
    g_runtime.procCount = procCount;
    g_runtime.trampolineAddr = (uint32_t)(uintptr_t)translator_trampoline | 1u;
    g_runtime.enterProgramReturn.code_ptr = (uint32_t)(uintptr_t)landing_point | 1u;
    g_runtime.enterProgramReturn.last_used = 0;

    for(uint32_t i = 0; i < procCount; i++)
    {
        g_runtime.dispatchTable[i].code_ptr = g_runtime.trampolineAddr;
        g_runtime.dispatchTable[i].last_used = 0;
    }

    g_helperVec[0] = callHelperAddr;
    g_helperVec[1] = returnHelperAddr;

    uint32_t dispatchBase = (uint32_t)(uintptr_t)&g_runtime.dispatchTable[0];
    uint32_t ctrlBase = (uint32_t)(uintptr_t)&g_ctrlStack[0];
    uint32_t helperVecBase = (uint32_t)(uintptr_t)&g_helperVec[0];
    uint32_t bootRecord = PACK_RECORD(-1, LANDING_SUCCESS);

    /* enter_program reusing callHelper for its own entry (this session's
     * simplification): push the bootstrap (-1, LANDING_SUCCESS) record
     * exactly as any real CALL site would push its own, then dispatch into
     * proc 0 — no separate dispatch-in path needed. */
    register uint32_t r8_ asm("r8") = dispatchBase;
    register uint32_t r9_ asm("r9") = ctrlBase;
    register uint32_t r10_ asm("r10") = helperVecBase;
    register uint32_t r11_ asm("r11") = 0;
    register uint32_t r0_ asm("r0") = argIn;
    register uint32_t r1_ asm("r1") = bootRecord;
    register uint32_t r2_ asm("r2") = 0; /* Q_idx = entry procedure */
    register uint32_t r3_ asm("r3") = callHelperAddr;

    asm volatile("bx r3"
        :
        : "r"(r8_), "r"(r9_), "r"(r10_), "r"(r11_), "r"(r0_), "r"(r1_), "r"(r2_), "r"(r3_));
    __builtin_unreachable();
}
