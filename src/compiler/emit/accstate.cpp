#include "accstate.h"
#include "assembler.h"

namespace jitc
{

using R = ArmV6M::LoReg;

/* ARMv6-M has no conditional-set, so a comparison's 0/1 costs a branch over a
 * pair of MOVS. Both arms end on one, so N/Z come out set from `dstReg`. */
Effect AccState::materializeBoolean(Assembler &e, uint32_t dstReg)
{
    Label falseLabel;
    const auto falseOk = e.branchTo(falseLabel, ArmV6M::inverse(flags.cond()));
    assert(falseOk);

    e.emit(ArmV6M::movs(R((uint16_t)dstReg), ArmV6M::Imm<8>(1)));

    Label endLabel;
    const auto endOk = e.branchTo(endLabel);
    assert(endOk);

    const auto falseBound = e.bind(falseLabel);
    assert(falseBound);

    e.emit(ArmV6M::movs(R((uint16_t)dstReg), ArmV6M::Imm<8>(0)));

    const auto endBound = e.bind(endLabel);
    assert(endBound);

    return Effect::into(dstReg, true);
}

void AccState::flushPending(Assembler &e, uint32_t dstReg)
{
    if(livesIn(dstReg))
    {
        return; // nothing emitted, so nothing about the flags can have moved
    }

    apply(value.materialize(e, dstReg));
    pending(Shape::ofReg(dstReg));
}

void AccState::flush(Assembler &e, uint32_t dstReg)
{
    assert(kind != Kind::Dead); // GCOV_EXCL_LINE — nothing to put anywhere

    if(kind == Kind::Boolean)
    {
        apply(materializeBoolean(e, dstReg));
        pending(Shape::ofReg(dstReg));
        return;
    }

    flushPending(e, dstReg);
}

uint32_t AccState::sourceReg(Assembler &e, uint32_t scratchReg)
{
    if(kind == Kind::Boolean)
    {
        materializeBoolean(e, scratchReg);
        return scratchReg;
    }

    return operand().sourceReg(e, scratchReg);
}

ArmV6M::Condition AccState::testNonzero(Assembler &e)
{
    // A comparison's own condition is "acc would be 1", which is exactly
    // "not zero" for a value that is only ever 0 or 1.
    if(kind == Kind::Boolean)
    {
        return flags.cond();
    }

    if(!value.isReg() || !flags.answersZeroOf(value.reg()))
    {
        const uint32_t r = sourceReg(e, SCRATCH_REG);
        e.emit(ArmV6M::cmp(R((uint16_t)r), ArmV6M::Imm<8>(0)));
        apply(Effect::into(r, true));
    }

    return ArmV6M::Condition::NE;
}

} // namespace jitc
