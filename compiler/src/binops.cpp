#include "binops.h"
#include "assembler.h"
#include "registers.h"
#include "imm_synth.h"
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
    else if(which != AddSubRsubOp::Rsub && fitsImm3(k))
    {
        /*
         * Small literal rhs version, no real rsub in Thumb-1.
         */
        e.emit(which == AddSubRsubOp::Sub
            ? ArmV6M::subs(R((uint16_t)dest), R((uint16_t)n), ArmV6M::Imm<3>((uint16_t)k))
            : ArmV6M::adds(R((uint16_t)dest), R((uint16_t)n), ArmV6M::Imm<3>((uint16_t)k)));
        
    }
    else if(which != AddSubRsubOp::Rsub && fitsImm8(k) && dest == n)
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
        /*
         * Can't fold constant, materialize into temporary instead, becomes temp = k, then dest = n + temp.
         */
        auto t = SCRATCH_REG;

        if(n == SCRATCH_REG)
        {
            /*
             * If input aliases SCRATCH_REG becomes dest = k, then dest += n.
             */
            assert(dest != SCRATCH_REG);
            t = dest;
        }
        
        e.materializeImm32(t, k);

        switch(which)
        {
            case AddSubRsubOp::Add:  e.emit(ArmV6M::adds(R((uint16_t)dest), R((uint16_t)n), R((uint16_t)t))); break;
            case AddSubRsubOp::Sub:  e.emit(ArmV6M::subs(R((uint16_t)dest), R((uint16_t)n), R((uint16_t)t))); break;
            case AddSubRsubOp::Rsub: e.emit(ArmV6M::subs(R((uint16_t)dest), R((uint16_t)t), R((uint16_t)n))); break;
        }
    }
}

void emitBinaryOp(Assembler &e, Op op, Combo combo, const Shape &accShape, const Shape *operand, uint32_t dest)
{
    Shape rhs = operand ? *operand : Shape::ofReg(dest);

    if(accShape.isImm && rhs.isImm)
    {
        /*
        * Degenerate case - foldable constants, lowerer should have caught it but easier 
        * to deal with it like this and is required for correctness.
        */
        switch (op)
        {
            case Op::ADD:  e.materializeImm32(dest, accShape.imm + rhs.imm); break;
            case Op::SUB:  e.materializeImm32(dest, accShape.imm - rhs.imm); break;
            case Op::RSUB: e.materializeImm32(dest, rhs.imm - accShape.imm); break;
            case Op::MUL:  e.materializeImm32(dest, accShape.imm * rhs.imm); break;
            case Op::AND:  e.materializeImm32(dest, accShape.imm & rhs.imm); break;
            case Op::OR:   e.materializeImm32(dest, accShape.imm | rhs.imm); break;
            case Op::XOR:  e.materializeImm32(dest, accShape.imm ^ rhs.imm); break;
            case Op::SHL:  e.materializeImm32(dest, accShape.imm << rhs.imm); break;
            case Op::SHR:  e.materializeImm32(dest, accShape.imm >> rhs.imm); break;
            case Op::ASR:  e.materializeImm32(dest, (int32_t)accShape.imm >> rhs.imm); break;
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
            assert(operand != nullptr && operand->isImm); // GCOV_EXCL_LINE

            const auto m = shapeToReg(e, accShape, SCRATCH_REG);

            switch(op)
            {
                case Op::SHL: e.emit(ArmV6M::lsls(R((uint16_t)dest), R((uint16_t)m), ArmV6M::Imm<5>((uint16_t)operand->imm))); break;
                case Op::SHR: e.emit(ArmV6M::lsrs(R((uint16_t)dest), R((uint16_t)m), ArmV6M::Imm<5>((uint16_t)operand->imm))); break;
                case Op::ASR: e.emit(ArmV6M::asrs(R((uint16_t)dest), R((uint16_t)m), ArmV6M::Imm<5>((uint16_t)operand->imm))); break;
                default: assert(false); // GCOV_EXCL_LINE
            }
        }
        else
        {
            /*
             * Shift by register, non-commutative, only a <<= b form isn exists, mov + shift (either order) 
             * in general, aliasing hazard.
             */
            auto m = shapeToReg(e, rhs, SCRATCH_REG);
            auto t = (m == dest) ? ACC_REG : dest;

            materializeShape(e, accShape, t);

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
        auto m = shapeToReg(e, rhs, SCRATCH_REG);

        if(m == dest)
        {
            const auto t = shapeToReg(e, accShape, ACC_REG);

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
            materializeShape(e, accShape, dest);

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

} // namespace jitc
