#include "ext.h"
#include "assembler.h"
#include "window.h"
#include "accstate.h"
#include "registers.h"
#include "armv6.h"

#include <cassert>

using namespace jitc;

namespace
{
void moveIfNeeded(Assembler &a, uint32_t dst, uint32_t src)
{
    if(dst != src)
    {
        a.emit(ArmV6M::mov(ArmV6M::AnyReg((uint16_t)dst), ArmV6M::AnyReg((uint16_t)src)));
    }
}
} // namespace

uint32_t ExtSite::depth() const
{
    return window.tos;
}

uint32_t ExtSite::load(uint32_t slot, uint32_t dstReg)
{
    if(inWindow(window.tos, slot))
    {
        return physReg(slot);
    }

    a.emit(ArmV6M::ldrSp(ArmV6M::LoReg((uint16_t)dstReg), spillImm(a, window.spillOffset(slot))));
    return dstReg;
}

void ExtSite::store(uint32_t slot, uint32_t srcReg)
{
    if(!inWindow(window.tos, slot))
    {
        a.emit(ArmV6M::strSp(ArmV6M::LoReg((uint16_t)srcReg), spillImm(a, window.spillOffset(slot))));
        return;
    }

    uint32_t dst = physReg(slot);
    acc.resolveIfLiveIn(a, dst);
    moveIfNeeded(a, dst, srcReg);
}

void ExtSite::push(uint32_t srcReg)
{
    acc.apply(window.pushFrom(a, acc, srcReg));
}

void ExtSite::pop(uint32_t dstReg)
{
    uint32_t src = window.topReg();

    acc.resolveIfLiveIn(a, dstReg);

    if(window.popUncovers())
    {
        acc.resolveIfLiveIn(a, src); // finishPop reloads this very register
    }

    moveIfNeeded(a, dstReg, src);
    acc.apply(window.finishPop(a)); // must run after the read above — same register
}

uint32_t ExtSite::accInto(uint32_t dstReg)
{
    acc.flush(a, dstReg);
    return dstReg;
}

void ExtSite::accIsNowIn(uint32_t reg)
{
    acc.pending(Shape::ofReg(reg));
}

void ExtSite::accInvalidate()
{
    acc.poison();
}

/* r0-r3 and r12 are gone across either reach, r0 among them, so the
 * accumulator does not survive one on its own. */
void ExtSite::helperCall(uint32_t helperAddr)
{
    assert(lrSaved); // GCOV_EXCL_LINE — a reach whose site never declared NEEDS_LR

    acc.poison();

    a.materializeImm32(ENTRY_JUMP_REG, helperAddr);
    a.emit(ArmV6M::blx(ArmV6M::AnyReg((uint16_t)ENTRY_JUMP_REG)));
    acc.apply(Effect::clobber());
}

void ExtSite::cHelperCall(uint32_t helperAddr)
{
    assert(lrSaved); // GCOV_EXCL_LINE — a reach whose site never declared NEEDS_LR

    acc.poison();
    acc.apply(Effect::clobber());

    a.materializeImm32(ENTRY_JUMP_REG, helperAddr);
    a.emit(ArmV6M::mov(ArmV6M::AnyReg(12), ArmV6M::AnyReg(ENTRY_JUMP_REG)));

    a.emit(ArmV6M::mov(ArmV6M::AnyReg(ENTRY_JUMP_REG), ArmV6M::AnyReg(HELPER_VEC_REG)));
    a.emit(ArmV6M::ldr(ArmV6M::LoReg(ENTRY_JUMP_REG), ArmV6M::LoReg(ENTRY_JUMP_REG),
        ArmV6M::Uoff<2, 5>((uint16_t)HELPER_EXT_THUNK_OFFSET)));
    a.emit(ArmV6M::blx(ArmV6M::AnyReg((uint16_t)ENTRY_JUMP_REG)));
}
