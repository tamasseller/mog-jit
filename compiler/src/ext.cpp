#include "ext.h"
#include "assembler.h"
#include "registers.h"
#include "armv6.h"

#include <cassert>

namespace jitc
{

namespace
{
void blxVia(Assembler &a, uint32_t reg)
{
    a.emit(ArmV6M::blx(ArmV6M::AnyReg((uint16_t)reg)));
}
} // namespace

void extEmitHelperCall(Assembler &a, const ExtSite &site, uint32_t helperAddr)
{
    assert(extDeclHas(site.decl, EXT_FLAG_NEEDS_LR)); // GCOV_EXCL_LINE
    (void)site;

    a.materializeImm32(ENTRY_JUMP_REG, helperAddr);
    blxVia(a, ENTRY_JUMP_REG);
}

void extEmitCHelperCall(Assembler &a, const ExtSite &site, uint32_t helperAddr)
{
    assert(extDeclHas(site.decl, EXT_FLAG_NEEDS_LR)); // GCOV_EXCL_LINE
    (void)site;

    a.materializeImm32(ENTRY_JUMP_REG, helperAddr);
    a.emit(ArmV6M::mov(ArmV6M::AnyReg(12), ArmV6M::AnyReg(ENTRY_JUMP_REG)));

    a.emit(ArmV6M::mov(ArmV6M::AnyReg(ENTRY_JUMP_REG), ArmV6M::AnyReg(HELPER_VEC_REG)));
    a.emit(ArmV6M::ldr(ArmV6M::LoReg(ENTRY_JUMP_REG), ArmV6M::LoReg(ENTRY_JUMP_REG),
        ArmV6M::Uoff<2, 5>((uint16_t)HELPER_EXT_THUNK_OFFSET)));
    blxVia(a, ENTRY_JUMP_REG);
}

} // namespace jitc
