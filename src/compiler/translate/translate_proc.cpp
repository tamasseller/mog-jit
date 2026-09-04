#include "translate_proc.h"
#include "translate_internal.h"
#include "runtime.h"

#include <cassert>
using namespace jitc;

Ctx::Ctx(Runtime& r, uint32_t procIdx, uint32_t lruTick): a(r, lruTick)
{
    const auto &procSlot = r.slot(procIdx);

    this->window = Window{procSlot.argCount(), procSlot.needsLRSave()};
    this->savesLR = procSlot.needsLRSave();
    this->procIdx = procIdx;
    this->initialSpilledCount = procSlot.argCount() > WINDOW_SIZE ? procSlot.argCount() - WINDOW_SIZE : 0;
    this->hasLookahead = false;

    /* The scan that produced bodyBytes could not be hinted; every pass after
     * it can, and there are at least two (§1.2). */
    bcHint(procSlot.bodyHandle, procSlot.bodyBytes());
    this->body.open(procSlot.bodyHandle, procSlot.bodyBytes());
}

extern "C" uint32_t translateProc(uint32_t procIdx, Runtime& r, uint32_t lruTick)
{
    if(Ctx ctx(r, procIdx, lruTick); ctx.translateBody(BranchWidth::Narrow))
    {
        return ctx.a.finalize(procIdx);
    }

    if(Ctx ctx(r, procIdx, lruTick); ctx.translateBody(BranchWidth::Wide))
    {
        return ctx.a.finalize(procIdx);
    }

    runtimeBail(&r, RESOURCE_LIMIT_BRANCH_RANGE);
    return -1;
}
