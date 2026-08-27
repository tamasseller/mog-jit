#include "unaryops.h"
#include "assembler.h"
#include "registers.h"
#include "armv6.h"

#include <cassert>

namespace jitc
{

using R = ArmV6M::LoReg;

void emitUnary(Assembler &e, Op op, uint32_t dest, uint32_t src)
{
    if(op == Op::NEG)
    {
        e.emit(ArmV6M::negs(R((uint16_t)dest), R((uint16_t)src)));
        return;
    }
    if(op == Op::NOT)
    {
        e.emit(ArmV6M::mvns(R((uint16_t)dest), R((uint16_t)src)));
        return;
    }

    assert(src == ACC_REG); // GCOV_EXCL_LINE — clzHelper/revbitsHelper hardcode ACC_REG, caller's job to flush there first

    // CLZ / REVBITS in the flash-resident helper vector (docs/design.md
    // §11). BLX rather than BX: both routines are ordinary subroutines
    // that return via `bx lr`, unlike the tail-jumping callHelper/
    // returnHelper* this same MOV/LDR idiom also reaches (abi_strategy.cpp's
    // own precedent).
    uint32_t offset = (op == Op::CLZ) ? HELPER_CLZ_OFFSET : HELPER_REVBITS_OFFSET;
    e.emit(ArmV6M::mov(ArmV6M::AnyReg(ENTRY_JUMP_REG), ArmV6M::AnyReg(HELPER_VEC_REG)));
    e.emit(ArmV6M::ldr(R(ENTRY_JUMP_REG), R(ENTRY_JUMP_REG), ArmV6M::Uoff<2, 5>((uint16_t)offset)));
    e.emit(ArmV6M::blx(ArmV6M::AnyReg(ENTRY_JUMP_REG)));
    if(dest != ACC_REG)
    {
        e.emit(ArmV6M::mov(ArmV6M::AnyReg((uint16_t)dest), ArmV6M::AnyReg((uint16_t)ACC_REG)));
    }
}

} // namespace jitc
