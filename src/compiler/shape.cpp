#include "shape.h"
#include "assembler.h"
#include "armv6.h"

namespace jitc
{

using R = ArmV6M::LoReg;

void Shape::materialize(Assembler &a, uint32_t dstReg) const
{
    if(this->isImm())
    {
        a.materializeImm32(dstReg, (uint32_t)this->imm());
    }
    else if(this->isFlags())
    {
        Label falseLabel;
        const auto falseOk = a.branchTo(falseLabel, ArmV6M::inverse(this->cond()));
        assert(falseOk);

        a.emit(ArmV6M::movs(R((uint16_t)dstReg), ArmV6M::Imm<8>(1)));

        Label endLabel;
        const auto endOk = a.branchTo(endLabel);
        assert(endOk);

        const auto falseBound = a.bind(falseLabel);
        assert(falseBound);

        a.emit(ArmV6M::movs(R((uint16_t)dstReg), ArmV6M::Imm<8>(0)));

        const auto endBound = a.bind(endLabel);
        assert(endBound);
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
