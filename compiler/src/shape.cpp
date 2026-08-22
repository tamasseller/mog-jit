#include "shape.h"
#include "emitter.h"
#include "imm_synth.h"
#include "armv6.h"

namespace jitc {

void materializeShape(Emitter &e, const Shape &shape, uint32_t dstReg) {
    if(shape.isImm) emitSynthesizeImm32(e, dstReg, (uint32_t)shape.imm);
    else if(shape.reg != dstReg) e.emit(ArmV6M::mov(ArmV6M::AnyReg((uint16_t)dstReg), ArmV6M::AnyReg((uint16_t)shape.reg)));
}

uint32_t shapeToReg(Emitter &e, const Shape &shape, uint32_t scratchReg) {
    if(!shape.isImm) return shape.reg;
    materializeShape(e, shape, scratchReg);
    return scratchReg;
}

} // namespace jitc
