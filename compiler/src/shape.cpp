#include "shape.h"
#include "assembler.h"
#include "armv6.h"

namespace jitc
{

void materializeShape(Assembler &a, const Shape &shape, uint32_t dstReg)
{
    if(shape.isImm)
    {
        a.materializeImm32(dstReg, (uint32_t)shape.imm);
    }
    else if(shape.reg != dstReg)
    {
        a.emit(ArmV6M::mov(ArmV6M::AnyReg((uint16_t)dstReg), ArmV6M::AnyReg((uint16_t)shape.reg)));
    }
}

uint32_t shapeToReg(Assembler &a, const Shape &shape, uint32_t scratchReg)
{
    if(!shape.isImm)
    {
        return shape.reg;
    }
    
    materializeShape(a, shape, scratchReg);
    return scratchReg;
}

} // namespace jitc
