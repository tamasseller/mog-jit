#include "shape.h"
#include "assembler.h"
#include "armv6.h"

namespace jitc
{

bool Shape::materialize(Assembler &a, uint32_t dstReg) const
{
    if(this->isImm())
    {
        return a.materializeImm32(dstReg, (uint32_t)this->imm());
    }

    if(this->reg() != dstReg)
    {
        a.emit(ArmV6M::mov(ArmV6M::AnyReg((uint16_t)dstReg), ArmV6M::AnyReg((uint16_t)this->reg())));
    }

    return false;
}

uint32_t Shape::sourceReg(Assembler &a, uint32_t scratchReg) const
{
    if(this->isReg())
    {
        return this->reg();
    }

    this->materialize(a, scratchReg);
    return scratchReg;
}

} // namespace jitc
