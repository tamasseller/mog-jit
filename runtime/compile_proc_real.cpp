/* The real compileProc — genuine bytecode-to-Thumb translation
 * (compiler/src/translate_proc.h), reading every procedure's own body
 * pointer/argCount/needsLRSave straight out of the whole-program directory
 * Runtime::init() already built (runtime_internal.h's ProcSlot) — never a
 * fixture-supplied stand-in. Reuses runtime_internal.h's Runtime/ProcSlot
 * and runtime_host.h's ProgramResult unmodified.
 *
 * Translates directly into the arena at the current arenaCursor — no
 * scratch buffer, no final memcpy. RuntimeArenaRoom lets the translator
 * grow that headroom mid-pass by evicting/compacting other resident
 * procedures (docs/design.md §11's "one compaction extension"), so a
 * procedure's own real size is bounded only by the real arena, not by an
 * arbitrary scratch capacity picked ahead of time.
 */
#include <stdint.h>
#include "runtime_internal.h"
#include "translate_proc.h"
#include "arena_room.h"
#include "emitter.h"

namespace
{

/* The one seam between the Runtime-agnostic translator and Runtime's own
 * arena/eviction machinery — a plain stack local per compileProc call,
 * never heap-allocated (arena_room.h's own header comment has why it
 * carries no virtual destructor). */
class RuntimeArenaRoom : public jitc::ArenaRoom
{
public:
    explicit RuntimeArenaRoom(Runtime *runtime) : runtime(runtime)
    {
    }

    void ensureRoom(jitc::Emitter &e, uint32_t neededHalfwords) override
    {
        if(e.remainingHalfwords() >= neededHalfwords)
        {
            return;
        }

        register uint32_t now asm("r11");
        uint32_t neededBytes = e.halfwordCount() * 2 + neededHalfwords * 2;
        while(runtime->arenaEnd - runtime->arenaCursor < neededBytes)
        {
            int victim = runtime->findEvictionVictim(now);
            if(victim < 0)
            {
                return; // can't free any more — Emitter::overflowed() catches the eventual shortfall
            }
            // The in-progress emitter's own base is always exactly
            // arenaCursor (nothing has bumped it — Runtime::allocate()
            // only ever runs once, on success), so evict()'s extended
            // tail range, covering e's own already-written bytes too,
            // keeps that invariant true on the other side.
            runtime->evict((uint32_t)victim, e.halfwordCount() * 2);
        }

        e.rebase((uint16_t *)(uintptr_t)runtime->arenaCursor, (runtime->arenaEnd - runtime->arenaCursor) / 2);
    }

private:
    Runtime *runtime;
};

} // namespace

static void __attribute__((noreturn)) bailOut(Runtime *runtime, uint32_t trapCode)
{
    register uint32_t trapCodeReg asm("r0") = trapCode;
    register uint32_t tagReg asm("r2") = LANDING_TRAP;
    register uint32_t landingReg asm("r3") = runtime->sentinelLandingAddress();
    register uint32_t savedSpReg asm("r1") = runtime->savedSp;
    asm volatile("mov sp, r1\n\tbx r3" : : "r"(trapCodeReg), "r"(savedSpReg), "r"(tagReg), "r"(landingReg));
    __builtin_unreachable();
}

extern "C" void compileProc(uint32_t idx, Runtime *runtime)
{
    const ProcSlot &procSlot = runtime->slot(idx);
    jitc::Proc proc{procSlot.argCount(), (const uint8_t *)(uintptr_t)procSlot.bodyPtr, procSlot.bodyBytes()};
    bool savesLR = procSlot.needsLRSave();

    /* abiEmitCall needs O(1) indexing by calleeIndex; ProcSlot's own
     * 16-byte stride doesn't give it that for free, so this dense copy —
     * a VLA, explicitly budgeted into requiredStackBytes
     * (runtime_host.cpp's CALLEE_ARG_COUNTS_BYTES_PER_PROC) rather than a
     * fixed cap, since a real program's own procCount isn't bounded to
     * any fixture-sized constant. */
    uint32_t calleeArgCounts[runtime->procCount];
    for(uint32_t i = 0; i < runtime->procCount; i++)
    {
        calleeArgCounts[i] = runtime->slot(i).argCount();
    }

    RuntimeArenaRoom room(runtime);

    /* Read fresh every call — arenaCursor (folded into liveStackFloor() for
     * enterProgramOnStack) moves between different procedures'
     * compilations, so this can't be cached anywhere once and reused. */
    jitc::TranslateResult result = jitc::translateProc(
        proc, idx, calleeArgCounts, runtime->procCount,
        (uint16_t *)(uintptr_t)runtime->arenaCursor, (runtime->arenaEnd - runtime->arenaCursor) / 2,
        runtime->liveStackFloor(), &savesLR, &room);
    if(result.overflowed)
    {
        bailOut(runtime, RESOURCE_ERROR_CODE);
    }

    uint32_t need = result.halfwordCount * 2;
    uint32_t dest = runtime->allocate(need);
    runtime->markCompiled(idx, dest);
}
