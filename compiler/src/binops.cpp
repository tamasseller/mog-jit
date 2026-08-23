#include "binops.h"
#include "emitter.h"
#include "registers.h"
#include "imm_synth.h"
#include "armv6.h"

#include <cassert>

namespace jitc
{

using R = ArmV6M::LoReg;

BinOpKind classifyBinOp(Op op, Combo combo)
{
    if(op == Op::ADD || op == Op::SUB || op == Op::RSUB)
    {
        return BinOpKind::AddSubRsub;
    }
    if((op == Op::SHL || op == Op::SHR || op == Op::ASR) && combo == Combo::IMM_ACC)
    {
        return BinOpKind::ShiftImm;
    }
    return BinOpKind::TwoOpInPlace; // AND, OR, XOR, MUL, and any shift with a register count
}

namespace
{

/** Rd = n +/- k, materializing k into scratch first when no native
 *  immediate form fits.
 *
 *  n isn't always a register dest is free to leave alone: a caller can
 *  pass rhs.reg straight through, and when that operand came from an
 *  out-of-window stack slot (translate_proc.cpp's own ldrSp(SCRATCH_REG,
 *  ...) reload), n == SCRATCH_REG. Materializing k into SCRATCH_REG right
 *  after would silently clobber that just-reloaded value before the final
 *  op ever reads it, computing `k op k` instead of `n op k` — copy n into
 *  dest first so it survives k's own materialization. */
void addOrSubWithImm(Emitter &e, bool sub, uint32_t dest, uint32_t n, int32_t k)
{
    if(fitsImm3(k))
    {
        e.emit(sub ? ArmV6M::subs(R((uint16_t)dest), R((uint16_t)n), ArmV6M::Imm<3>((uint16_t)k))
                   : ArmV6M::adds(R((uint16_t)dest), R((uint16_t)n), ArmV6M::Imm<3>((uint16_t)k)));
        return;
    }
    if(fitsImm8(k) && dest == n)
    {
        e.emit(sub ? ArmV6M::subs(R((uint16_t)dest), ArmV6M::Imm<8>((uint16_t)k))
                   : ArmV6M::adds(R((uint16_t)dest), ArmV6M::Imm<8>((uint16_t)k)));
        return;
    }
    if(n == SCRATCH_REG)
    {
        materializeShape(e, Shape::ofReg(n), dest);
        n = dest;
    }
    materializeShape(e, Shape::ofImm(k), SCRATCH_REG);
    e.emit(sub ? ArmV6M::subs(R((uint16_t)dest), R((uint16_t)n), R((uint16_t)SCRATCH_REG))
               : ArmV6M::adds(R((uint16_t)dest), R((uint16_t)n), R((uint16_t)SCRATCH_REG)));
}

/** Rd = k - n — immediate minus register, which has no direct native
 *  form: k===0 degenerates to NEG; otherwise materialize k into scratch.
 *  Same n == SCRATCH_REG hazard as addOrSubWithImm above, same fix. */
void emitRsubImmAsLeft(Emitter &e, uint32_t dest, int32_t k, uint32_t n)
{
    if(k == 0)
    {
        e.emit(ArmV6M::negs(R((uint16_t)dest), R((uint16_t)n)));
        return;
    }
    if(n == SCRATCH_REG)
    {
        materializeShape(e, Shape::ofReg(n), dest);
        n = dest;
    }
    materializeShape(e, Shape::ofImm(k), SCRATCH_REG);
    e.emit(ArmV6M::subs(R((uint16_t)dest), R((uint16_t)SCRATCH_REG), R((uint16_t)n)));
}

/** ADD/SUB/RSUB, covering every (accShape, operandShape) combination —
 *  operand===nullptr means PEEK_PEEK, whose right-hand operand is dest
 *  itself. */
void emitAddSubRsub(Emitter &e, Op op, uint32_t dest, const Shape &accShape, const Shape *operand)
{
    Shape rhs = operand ? *operand : Shape::ofReg(dest);

    if(op == Op::ADD)
    {
        if(!accShape.isImm)
        {
            if(!rhs.isImm)
            {
                e.emit(ArmV6M::adds(R((uint16_t)dest), R((uint16_t)accShape.reg), R((uint16_t)rhs.reg)));
            }
            else
            {
                addOrSubWithImm(e, false, dest, accShape.reg, rhs.imm);
            }
        }
        else if(!rhs.isImm)
        {
            addOrSubWithImm(e, false, dest, rhs.reg, accShape.imm);
        }
        else
        {
            // both imm — rare/degenerate. Materialize accShape into dest
            // itself, not SCRATCH_REG: addOrSubWithImm's own fallback below
            // (when k doesn't fit imm3/imm8) also materializes k into
            // SCRATCH_REG, and dest is guaranteed distinct from
            // SCRATCH_REG at every call site that can reach this branch
            // (registers.h's own invariant — SCRATCH_REG is never a
            // window register, never acc's own home), so the two can't
            // alias.
            materializeShape(e, accShape, dest);
            addOrSubWithImm(e, false, dest, dest, rhs.imm);
        }
        return;
    }

    if(op == Op::SUB)
    { // acc - rhs
        if(!accShape.isImm)
        {
            if(!rhs.isImm)
            {
                e.emit(ArmV6M::subs(R((uint16_t)dest), R((uint16_t)accShape.reg), R((uint16_t)rhs.reg)));
            }
            else
            {
                addOrSubWithImm(e, true, dest, accShape.reg, rhs.imm);
            }
        }
        else if(!rhs.isImm)
        {
            emitRsubImmAsLeft(e, dest, accShape.imm, rhs.reg);
        }
        else
        {
            materializeShape(e, accShape, dest); // both imm — see ADD's own comment above
            addOrSubWithImm(e, true, dest, dest, rhs.imm);
        }
        return;
    }

    // RSUB: rhs - acc
    if(!rhs.isImm)
    {
        if(!accShape.isImm)
        {
            e.emit(ArmV6M::subs(R((uint16_t)dest), R((uint16_t)rhs.reg), R((uint16_t)accShape.reg)));
        }
        else
        {
            // acc imm (k), rhs reg (n): dest = n - k — an ordinary
            // register-minus-immediate, the ADD/SUB row's own fold.
            addOrSubWithImm(e, true, dest, rhs.reg, accShape.imm);
        }
    }
    else if(!accShape.isImm)
    {
        emitRsubImmAsLeft(e, dest, rhs.imm, accShape.reg);
    }
    else
    {
        materializeShape(e, accShape, dest); // both imm — see ADD's own comment above
        emitRsubImmAsLeft(e, dest, rhs.imm, dest);
    }
}

uint16_t shiftOpImm(Op op, uint32_t d, uint32_t m, int32_t amount)
{
    if(op == Op::SHL)
    {
        return ArmV6M::lsls(R((uint16_t)d), R((uint16_t)m), ArmV6M::Imm<5>((uint16_t)amount));
    }
    if(op == Op::SHR)
    {
        return ArmV6M::lsrs(R((uint16_t)d), R((uint16_t)m), ArmV6M::Imm<5>((uint16_t)amount));
    }
    return ArmV6M::asrs(R((uint16_t)d), R((uint16_t)m), ArmV6M::Imm<5>((uint16_t)amount));
}

uint16_t twoOpInPlaceNative(Op op, uint32_t dn, uint32_t m)
{
    switch(op)
    {
        case Op::AND: return ArmV6M::ands(R((uint16_t)dn), R((uint16_t)m));
        case Op::OR:  return ArmV6M::orrs(R((uint16_t)dn), R((uint16_t)m));
        case Op::XOR: return ArmV6M::eors(R((uint16_t)dn), R((uint16_t)m));
        case Op::MUL: return ArmV6M::muls(R((uint16_t)dn), R((uint16_t)m));
        case Op::SHL: return ArmV6M::lsls(R((uint16_t)dn), R((uint16_t)m));
        case Op::SHR: return ArmV6M::lsrs(R((uint16_t)dn), R((uint16_t)m));
        case Op::ASR: return ArmV6M::asrs(R((uint16_t)dn), R((uint16_t)m));
        default:
            assert(false); // GCOV_EXCL_LINE
            return 0;      // GCOV_EXCL_LINE
    }
}

} // namespace

void emitBinaryOp(
    Emitter &e, Op op, Combo combo,
    const Shape &accShape, const Shape *operand, uint32_t dest)
{
    BinOpKind kind = classifyBinOp(op, combo);

    if(kind == BinOpKind::TwoOpInPlace)
    {
        // operand == nullptr means PEEK_PEEK — its own right-hand operand
        // is dest itself, the same idiom emitAddSubRsub already relies on
        // above: dest is safe to read as Rm since the native 2-op-in-place
        // form reads Rm before overwriting Rdn, and Rdn here is always
        // ACC_REG, never dest.
        Shape rhs = operand ? *operand : Shape::ofReg(dest);
        // Never folds — materialize acc into ACC_REG specifically,
        // unconditionally, even when accShape is already some other
        // register: the native 2-op-in-place form's Rdn slot is both read
        // and written, and that other register can be a live variable's
        // own home.
        materializeShape(e, accShape, ACC_REG);
        uint32_t m = shapeToReg(e, rhs, SCRATCH_REG);
        e.emit(twoOpInPlaceNative(op, ACC_REG, m));
        if(dest != ACC_REG)
        {
            e.emit(ArmV6M::mov(ArmV6M::AnyReg((uint16_t)dest), ArmV6M::AnyReg((uint16_t)ACC_REG)));
        }
        return;
    }

    if(kind == BinOpKind::ShiftImm)
    {
        assert(operand != nullptr && operand->isImm); // GCOV_EXCL_LINE
        e.emit(shiftOpImm(op, dest, shapeToReg(e, accShape, SCRATCH_REG), operand->imm));
        return;
    }

    emitAddSubRsub(e, op, dest, accShape, operand);
}

} // namespace jitc
