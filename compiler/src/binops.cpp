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
         * Can't fold constant, materialize into a temporary instead: temp = k, then dest = n op temp.
         */
        auto t = SCRATCH_REG;

        if(n == SCRATCH_REG)
        {
            /*
             * n already occupies SCRATCH_REG, so it can't also hold the
             * materialized constant -- and dest can't stand in for it
             * either, since dest can itself alias SCRATCH_REG here (e.g. a
             * spilled REG_REG operand loaded into SCRATCH_REG, combined
             * with an immediate accumulator, and stored back out through
             * SCRATCH_REG too). ENTRY_JUMP_REG is never live across
             * bytecode instructions -- only used transiently by
             * CALL/RETURN/BR_TABLE dispatch sequences -- so it's always
             * free to borrow as the temporary here.
             */
            t = ENTRY_JUMP_REG;
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

void emitBinaryOp(Assembler &e, Op op, Combo combo, const Shape &accShape, const Shape &rhs, uint32_t dest)
{
    if(accShape.isImm && rhs.isImm)
    {
        /*
        * Degenerate case - foldable constants, lowerer should have caught it but easier 
        * to deal with it like this and is required for correctness.
        */
        // ADD/SUB/RSUB/MUL/SHL/SHR are all done in uint32_t: isa-core.md's
        // arithmetic wraps modulo 2^32 (matching vm.ts's `| 0`/`>>> 0`
        // reference semantics and real ARM instructions), but accShape.imm
        // /rhs.imm are int32_t, and signed overflow (ADD/SUB/RSUB/MUL) or
        // shifting a negative value (SHL) is undefined behavior in C++,
        // not just a different result -- unlike the equivalent register
        // forms below, which land on real wrapping ARM instructions
        // regardless of signedness. SHR is a logical shift, so it also
        // needs the unsigned cast to avoid sign-extending a negative
        // accShape.imm; ASR already casts (the other way) to get the
        // sign-extension it actually wants.
        switch (op)
        {
            case Op::ADD:  e.materializeImm32(dest, (uint32_t)accShape.imm + (uint32_t)rhs.imm); break;
            case Op::SUB:  e.materializeImm32(dest, (uint32_t)accShape.imm - (uint32_t)rhs.imm); break;
            case Op::RSUB: e.materializeImm32(dest, (uint32_t)rhs.imm - (uint32_t)accShape.imm); break;
            case Op::MUL:  e.materializeImm32(dest, (uint32_t)accShape.imm * (uint32_t)rhs.imm); break;
            case Op::AND:  e.materializeImm32(dest, accShape.imm & rhs.imm); break;
            case Op::OR:   e.materializeImm32(dest, accShape.imm | rhs.imm); break;
            case Op::XOR:  e.materializeImm32(dest, accShape.imm ^ rhs.imm); break;
            // validate.ts rejects an immediate shift amount outside
            // 0..31 (isa-core.md §4.1), so the mask is only ever a no-op
            // on a validated program -- kept because it is free at compile
            // time and because an unmasked count of 32+ would be undefined
            // behavior for C++'s own `<<`/`>>` right here in the
            // translator, which is a far worse failure than a wrong
            // constant.
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

            // The amount is known here, and validate.ts already rejects
            // one outside 0..31 (isa-core.md §4.1) -- but bytecode carries
            // a full u32 immediate, and this must not hand something wider
            // to Imm<5>'s own 5-bit field however it got here. Free at
            // compile time either way.
            //
            // LSR/ASR's *immediate* encoding is the one real wrinkle, and
            // is unrelated to any of that: imm5==0 means "shift by 32"
            // there (unlike LSL, where imm5==0 already means a genuine
            // no-op), so a shift of 0 has to become a plain register move
            // instead of lsrs/asrs #0.
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

            // A bare register-form shift, amount unmasked. ARMv6-M reads
            // Rm[7:0] here, not Rm[4:0], so an amount of 32 or more does
            // not agree with the five-bit masking a host `<<` would do --
            // and isa-core.md §4.1 leaves exactly that case unspecified,
            // so there is nothing to agree with. Masking it would cost
            // LSLS #27 / LSRS #27 into a scratch on every dynamic shift
            // (ARMv6-M has no AND-with-immediate, so an AND against 31
            // would cost a register to hold the constant and be worse
            // still) -- three instructions where one does the job, to pin
            // down a case no codec depends on. The immediate combo above
            // is a separate matter: the amount is known there, validate.ts
            // rejects one outside 0..31 outright, and masking it costs
            // nothing at compile time.
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

} // namespace jitc
