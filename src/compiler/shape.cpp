#include "shape.h"
#include "assembler.h"
#include "armv6.h"

namespace jitc
{

void Shape::materialize(Assembler &a, uint32_t dstReg) const
{
    if(this->isImm())
    {
        a.materializeImm32(dstReg, (uint32_t)this->imm());
    }
    else if(this->reg() != dstReg)
    {
        a.emit(ArmV6M::mov(ArmV6M::AnyReg((uint16_t)dstReg), ArmV6M::AnyReg((uint16_t)this->reg())));
    }
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
