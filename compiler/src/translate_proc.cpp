#include "translate_proc.h"
#include "translate_internal.h"
#include "runtime_internal.h"

#include <cassert>
using namespace jitc;

Ctx::Ctx(Runtime& r, uint32_t procIdx, uint32_t lruTick): a(r, lruTick)
{
    const auto &procSlot = r.slot(procIdx);

    this->window = Window{procSlot.argCount(), procSlot.needsLRSave()};
    this->bytes = (const uint8_t *)(uintptr_t)procSlot.bodyPtr;
    this->bytesLen = procSlot.bodyBytes();
    this->savesLR = procSlot.needsLRSave();
    this->procIdx = procIdx; 
    this->initialSpilledCount = procSlot.argCount() > WINDOW_SIZE ? procSlot.argCount() - WINDOW_SIZE : 0;
}

uint32_t jitc::translateProc(uint32_t procIdx, Runtime& r, uint32_t lruTick)
{
    if(Ctx ctx(r, procIdx, lruTick); ctx.translateBody(emitNarrowBranch))
    {
        return ctx.a.finalize(procIdx);
    }

    if(Ctx ctx(r, procIdx, lruTick); ctx.translateBody(emitWideBranch))
    {
        return ctx.a.finalize(procIdx);
    }

    runtimeBail(&r, RESOURCE_LIMIT_BRANCH_RANGE);
    return -1;
}

