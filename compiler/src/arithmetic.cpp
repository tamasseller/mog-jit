#include "arithmetic.h"
#include "assembler.h"
#include "registers.h"
#include "armv6.h"

#include <cassert>

namespace jitc
{

using R = ArmV6M::LoReg;

enum class AddSubRsubOp
{
    Add, Sub, Rsub
};

static void addOrSubWithImm(AddSubRsubOp which, Assembler &e, uint32_t dest, uint32_t n, int32_t k)
{
    if(k == 0)
    {
        /*
         * Degenerate case: constant is zero, lowerer should have caught it. But still
         * needs to be handled for correctness.
         */
        if(which != AddSubRsubOp::Rsub)
        {
            /*
             * Add or take away zero is nop, except if destination is different.
             */
            if(dest != n)
            {
                e.emit(ArmV6M::mov(R((uint16_t)dest), R((uint16_t)n)));
            }
        }
        else
        {
            /*
             * Substract from zero is negation, source vs destination match doesn't matter.
             */
            e.emit(ArmV6M::negs(R((uint16_t)dest), R((uint16_t)n)));
        }
    }
    else if(which != AddSubRsubOp::Rsub && ArmV6M::fitsImm3(k))
    {
        /*
         * Small literal rhs version, no real rsub in Thumb-1.
         */
        e.emit(which == AddSubRsubOp::Sub
            ? ArmV6M::subs(R((uint16_t)dest), R((uint16_t)n), ArmV6M::Imm<3>((uint16_t)k))
            : ArmV6M::adds(R((uint16_t)dest), R((uint16_t)n), ArmV6M::Imm<3>((uint16_t)k)));
        
    }
    else if(which != AddSubRsubOp::Rsub && ArmV6M::fitsImm8(k) && dest == n)
    {
        /*
         * Increment/decrement by literal version.
         */

        e.emit(which == AddSubRsubOp::Sub
            ? ArmV6M::subs(R((uint16_t)dest), ArmV6M::Imm<8>((uint16_t)k))
            : ArmV6M::adds(R((uint16_t)dest), ArmV6M::Imm<8>((uint16_t)k)));
    }
    else
    {
        const auto t = (n == SCRATCH_REG) ? ENTRY_JUMP_REG : SCRATCH_REG;

        e.materializeImm32(t, k);

        switch(which)
        {
            case AddSubRsubOp::Add:  e.emit(ArmV6M::adds(R((uint16_t)dest), R((uint16_t)n), R((uint16_t)t))); break;
            case AddSubRsubOp::Sub:  e.emit(ArmV6M::subs(R((uint16_t)dest), R((uint16_t)n), R((uint16_t)t))); break;
            case AddSubRsubOp::Rsub: e.emit(ArmV6M::subs(R((uint16_t)dest), R((uint16_t)t), R((uint16_t)n))); break;
        }
    }
}

void emitBinaryOp(Assembler &e, Op op, Combo combo, const Shape &accShape, const Shape &rhs, uint32_t dest)
{
    if(accShape.isImm && rhs.isImm)
    {
        /*
        * Degenerate case - foldable constants, lowerer should have caught it but easier 
        * to deal with it like this and is required for correctness.
        */
        switch (op)
        {
            case Op::ADD:  e.materializeImm32(dest, (uint32_t)accShape.imm + (uint32_t)rhs.imm); break;
            case Op::SUB:  e.materializeImm32(dest, (uint32_t)accShape.imm - (uint32_t)rhs.imm); break;
            case Op::RSUB: e.materializeImm32(dest, (uint32_t)rhs.imm - (uint32_t)accShape.imm); break;
            case Op::MUL:  e.materializeImm32(dest, (uint32_t)accShape.imm * (uint32_t)rhs.imm); break;
            case Op::AND:  e.materializeImm32(dest, accShape.imm & rhs.imm); break;
            case Op::OR:   e.materializeImm32(dest, accShape.imm | rhs.imm); break;
            case Op::XOR:  e.materializeImm32(dest, accShape.imm ^ rhs.imm); break;
            case Op::SHL:  e.materializeImm32(dest, (uint32_t)accShape.imm << (rhs.imm & 31)); break;
            case Op::SHR:  e.materializeImm32(dest, (uint32_t)accShape.imm >> (rhs.imm & 31)); break;
            case Op::ASR:  e.materializeImm32(dest, (uint32_t)((int32_t)accShape.imm >> (rhs.imm & 31))); break;
            default: assert(false);
        }
    }
    else if(op == Op::ADD || op == Op::SUB || op == Op::RSUB)
    {
        /*
         * Addition and substraction are well supported, many options here.
         */
        if(!accShape.isImm && !rhs.isImm)
        {
            /*
             * If both are registers the most general form is used.
             */
            switch(op)
            {
                case Op::ADD:  e.emit(ArmV6M::adds(R((uint16_t)dest), R((uint16_t)accShape.reg), R((uint16_t)rhs.reg))); break;
                case Op::SUB:  e.emit(ArmV6M::subs(R((uint16_t)dest), R((uint16_t)accShape.reg), R((uint16_t)rhs.reg))); break;
                case Op::RSUB: e.emit(ArmV6M::subs(R((uint16_t)dest), R((uint16_t)rhs.reg), R((uint16_t)accShape.reg))); break;
                default: assert(false); // GCOV_EXCL_LINE
            }
        }
        else if(rhs.isImm)
        {
            /*
             * Right hand side is constant left is register, asymmetric, SUB vs RSUB distinction must be kept.
             */
            switch(op)
            {
                case Op::ADD:  addOrSubWithImm(AddSubRsubOp::Add, e, dest, accShape.reg, rhs.imm); break;
                case Op::SUB:  addOrSubWithImm(AddSubRsubOp::Sub, e, dest, accShape.reg, rhs.imm); break;
                case Op::RSUB: addOrSubWithImm(AddSubRsubOp::Rsub, e, dest, accShape.reg, rhs.imm); break;
                default: assert(false); // GCOV_EXCL_LINE
            }
        }
        else
        {
            /*
             * Left hand side is constant, still asymmetric, but can use the same templates as the above,
             * with SUB vs RSUB exchanged.
             */
            assert(accShape.isImm);
            switch(op)
            {
                case Op::ADD:  addOrSubWithImm(AddSubRsubOp::Add, e, dest, rhs.reg, accShape.imm); break;
                case Op::SUB:  addOrSubWithImm(AddSubRsubOp::Rsub, e, dest, rhs.reg, accShape.imm); break;
                case Op::RSUB: addOrSubWithImm(AddSubRsubOp::Sub, e, dest, rhs.reg, accShape.imm); break;
                default: assert(false); // GCOV_EXCL_LINE
            }
        }
    }
    else if(op == Op::SHL || op == Op::SHR || op == Op::ASR)
    {
        if(combo == Combo::IMM_ACC)
        {
            /*
             * Shift by literal is also well supported, some special cases here.
             */
            assert(rhs.isImm); // GCOV_EXCL_LINE

            const auto m = accShape.peek(e, SCRATCH_REG);

            uint32_t shift = (uint32_t)rhs.imm & 31u;
            switch(op)
            {
                case Op::SHL:
                    e.emit(ArmV6M::lsls(R((uint16_t)dest), R((uint16_t)m), ArmV6M::Imm<5>((uint16_t)shift)));
                    break;
                case Op::SHR:
                    if(shift == 0) e.emit(ArmV6M::mov(ArmV6M::AnyReg(dest), ArmV6M::AnyReg(m)));
                    else e.emit(ArmV6M::lsrs(R((uint16_t)dest), R((uint16_t)m), ArmV6M::Imm<5>((uint16_t)shift)));
                    break;
                case Op::ASR:
                    if(shift == 0) e.emit(ArmV6M::mov(ArmV6M::AnyReg(dest), ArmV6M::AnyReg(m)));
                    else e.emit(ArmV6M::asrs(R((uint16_t)dest), R((uint16_t)m), ArmV6M::Imm<5>((uint16_t)shift)));
                    break;
                default: assert(false); // GCOV_EXCL_LINE
            }
        }
        else
        {
            /*
             * Shift by register, non-commutative, only a <<= b form isn exists, mov + shift (either order) 
             * in general, aliasing hazard.
             */
            auto m = rhs.peek(e, SCRATCH_REG);
            auto t = (m == dest) ? ACC_REG : dest;

            accShape.materialize(e, t);

            switch(op)
            {
                case Op::SHL: e.emit(ArmV6M::lsls(R((uint16_t)t), R((uint16_t)m))); break;
                case Op::SHR: e.emit(ArmV6M::lsrs(R((uint16_t)t), R((uint16_t)m))); break;
                case Op::ASR: e.emit(ArmV6M::asrs(R((uint16_t)t), R((uint16_t)m))); break;
                default: assert(false); // GCOV_EXCL_LINE
            }

            if(dest != t)
            {
                e.emit(ArmV6M::mov(ArmV6M::AnyReg((uint16_t)dest), ArmV6M::AnyReg((uint16_t)t)));
            }
        }
    }
    else
    {
        /*
         * Commutative register ops, only a *= b form isn exists, mov + op in that order in general, 
         * aliasing handled by swapping.
         */
        auto m = rhs.peek(e, SCRATCH_REG);

        if(m == dest)
        {
            const auto t = accShape.peek(e, ACC_REG);

            switch(op)
            {
                case Op::AND: e.emit(ArmV6M::ands(R((uint16_t)dest), R((uint16_t)t))); break;
                case Op::OR:  e.emit(ArmV6M::orrs(R((uint16_t)dest), R((uint16_t)t))); break;
                case Op::XOR: e.emit(ArmV6M::eors(R((uint16_t)dest), R((uint16_t)t))); break;
                case Op::MUL: e.emit(ArmV6M::muls(R((uint16_t)dest), R((uint16_t)t))); break;
                default: assert(false); // GCOV_EXCL_LINE
            }
        }
        else
        {
            accShape.materialize(e, dest);

            switch(op)
            {
                case Op::AND: e.emit(ArmV6M::ands(R((uint16_t)dest), R((uint16_t)m))); break;
                case Op::OR:  e.emit(ArmV6M::orrs(R((uint16_t)dest), R((uint16_t)m))); break;
                case Op::XOR: e.emit(ArmV6M::eors(R((uint16_t)dest), R((uint16_t)m))); break;
                case Op::MUL: e.emit(ArmV6M::muls(R((uint16_t)dest), R((uint16_t)m))); break;
                default: assert(false); // GCOV_EXCL_LINE
            }
        }
    }
}

static constexpr ArmV6M::Condition DIRECT_CONDITION[10] = 
{
    ArmV6M::Condition::EQ, 
    ArmV6M::Condition::NE, 
    ArmV6M::Condition::LT, 
    ArmV6M::Condition::LE, 
    ArmV6M::Condition::GT, 
    ArmV6M::Condition::GE, 
    ArmV6M::Condition::LO, 
    ArmV6M::Condition::LS, 
    ArmV6M::Condition::HI, 
    ArmV6M::Condition::HS,
};

static constexpr ArmV6M::Condition MIRRORED_CONDITION[10] = 
{
    ArmV6M::Condition::EQ, 
    ArmV6M::Condition::NE, 
    ArmV6M::Condition::GT, 
    ArmV6M::Condition::GE, 
    ArmV6M::Condition::LT, 
    ArmV6M::Condition::LE, 
    ArmV6M::Condition::HI, 
    ArmV6M::Condition::HS, 
    ArmV6M::Condition::LO, 
    ArmV6M::Condition::LS,
};

ArmV6M::Condition emitComparison(Assembler &a, Shape left, Op op, const Shape &operand)
{
    assert(isComparisonOp(op)); // GCOV_EXCL_LINE — translate_proc.cpp's own caller already checked
    uint32_t idx = (uint32_t)op - (uint32_t)Op::EQ;
    ArmV6M::Condition condition = DIRECT_CONDITION[idx];

    if(left.isImm && !operand.isImm && ArmV6M::fitsImm8(left.imm))
    {
        a.emit(ArmV6M::cmp(R((uint16_t)operand.reg), ArmV6M::Imm<8>((uint16_t)left.imm)));
        return MIRRORED_CONDITION[idx];
    }

    if(left.isImm)
    {
        left.materialize(a, ACC_REG);
        left = Shape::ofReg(ACC_REG);
    }

    if(!operand.isImm)
    {
        a.emit(ArmV6M::cmp(R((uint16_t)left.reg), R((uint16_t)operand.reg)));
    }
    else if(ArmV6M::fitsImm8(operand.imm))
    {
        a.emit(ArmV6M::cmp(R((uint16_t)left.reg), ArmV6M::Imm<8>((uint16_t)operand.imm)));
    }
    else
    {
        operand.materialize(a, SCRATCH_REG);
        a.emit(ArmV6M::cmp(R((uint16_t)left.reg), R((uint16_t)SCRATCH_REG)));
    }
    return condition;
}

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
