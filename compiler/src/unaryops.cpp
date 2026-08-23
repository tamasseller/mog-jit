#include "unaryops.h"
#include "emitter.h"
#include "registers.h"
#include "armv6.h"

namespace jitc
{

using R = ArmV6M::LoReg;

void emitUnary(Emitter &e, Op op, uint32_t dest)
{
    if(op == Op::NEG)
    {
        e.emit(ArmV6M::negs(R((uint16_t)dest), R(ACC_REG)));
        return;
    }
    if(op == Op::NOT)
    {
        e.emit(ArmV6M::mvns(R((uint16_t)dest), R(ACC_REG)));
        return;
    }

    // CLZ (index 4) / REVBITS (index 5) in the flash-resident helper vector
    // (docs/design.md §11). BLX rather than BX: both routines are ordinary
    // subroutines that return via `bx lr`, unlike the tail-jumping
    // callHelper/returnHelper* this same MOV/LDR idiom also reaches
    // (abi_strategy.cpp's own precedent).
    uint32_t offset = (op == Op::CLZ) ? 16 : 20;
    e.emit(ArmV6M::mov(ArmV6M::AnyReg(ENTRY_JUMP_REG), ArmV6M::AnyReg(HELPER_VEC_REG)));
    e.emit(ArmV6M::ldr(R(ENTRY_JUMP_REG), R(ENTRY_JUMP_REG), ArmV6M::Uoff<2, 5>((uint16_t)offset)));
    e.emit(ArmV6M::blx(ArmV6M::AnyReg(ENTRY_JUMP_REG)));
    if(dest != ACC_REG)
    {
        e.emit(ArmV6M::mov(ArmV6M::AnyReg((uint16_t)dest), ArmV6M::AnyReg((uint16_t)ACC_REG)));
    }
}

} // namespace jitc
