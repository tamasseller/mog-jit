#include "binops.h"
#include "emitter.h"
#include "registers.h"
#include "imm_synth.h"
#include "armv6.h"

#include <cassert>

namespace jitc {

using R = ArmV6M::LoReg;

BinOpKind classifyBinOp(Op op, Combo combo) {
    if(op == Op::ADD || op == Op::SUB || op == Op::RSUB) return BinOpKind::AddSubRsub;
    if((op == Op::SHL || op == Op::SHR || op == Op::ASR) && combo == Combo::IMM_ACC) return BinOpKind::ShiftImm;
    return BinOpKind::TwoOpInPlace; // AND, OR, XOR, MUL, and any shift with a register count
}

namespace {

/** Rd = n +/- k, materializing k into scratch first when no native
 *  immediate form fits. */
void addOrSubWithImm(Emitter &e, bool sub, uint32_t dest, uint32_t n, int32_t k) {
    if(fitsImm3(k)) {
        e.emit(sub ? ArmV6M::subs(R((uint16_t)dest), R((uint16_t)n), ArmV6M::Imm<3>((uint16_t)k))
                   : ArmV6M::adds(R((uint16_t)dest), R((uint16_t)n), ArmV6M::Imm<3>((uint16_t)k)));
        return;
    }
    if(fitsImm8(k) && dest == n) {
        e.emit(sub ? ArmV6M::subs(R((uint16_t)dest), ArmV6M::Imm<8>((uint16_t)k))
                   : ArmV6M::adds(R((uint16_t)dest), ArmV6M::Imm<8>((uint16_t)k)));
        return;
    }
    materializeShape(e, Shape::ofImm(k), SCRATCH_REG);
    e.emit(sub ? ArmV6M::subs(R((uint16_t)dest), R((uint16_t)n), R((uint16_t)SCRATCH_REG))
               : ArmV6M::adds(R((uint16_t)dest), R((uint16_t)n), R((uint16_t)SCRATCH_REG)));
}

/** Rd = k - n — immediate minus register, which has no direct native
 *  form: k===0 degenerates to NEG; otherwise materialize k into scratch. */
void emitRsubImmAsLeft(Emitter &e, uint32_t dest, int32_t k, uint32_t n) {
    if(k == 0) { e.emit(ArmV6M::negs(R((uint16_t)dest), R((uint16_t)n))); return; }
    materializeShape(e, Shape::ofImm(k), SCRATCH_REG);
    e.emit(ArmV6M::subs(R((uint16_t)dest), R((uint16_t)SCRATCH_REG), R((uint16_t)n)));
}

/** ADD/SUB/RSUB, covering every (accShape, operandShape) combination —
 *  operand===nullptr means PEEK_PEEK, whose right-hand operand is dest
 *  itself. */
void emitAddSubRsub(Emitter &e, Op op, uint32_t dest, const Shape &accShape, const Shape *operand) {
    Shape rhs = operand ? *operand : Shape::ofReg(dest);

    if(op == Op::ADD) {
        if(!accShape.isImm) {
            if(!rhs.isImm) e.emit(ArmV6M::adds(R((uint16_t)dest), R((uint16_t)accShape.reg), R((uint16_t)rhs.reg)));
            else addOrSubWithImm(e, false, dest, accShape.reg, rhs.imm);
        } else if(!rhs.isImm) {
            addOrSubWithImm(e, false, dest, rhs.reg, accShape.imm);
        } else {
            addOrSubWithImm(e, false, dest, shapeToReg(e, accShape, SCRATCH_REG), rhs.imm); // both imm — rare/degenerate
        }
        return;
    }

    if(op == Op::SUB) { // acc - rhs
        if(!accShape.isImm) {
            if(!rhs.isImm) e.emit(ArmV6M::subs(R((uint16_t)dest), R((uint16_t)accShape.reg), R((uint16_t)rhs.reg)));
            else addOrSubWithImm(e, true, dest, accShape.reg, rhs.imm);
        } else if(!rhs.isImm) {
            emitRsubImmAsLeft(e, dest, accShape.imm, rhs.reg);
        } else {
            addOrSubWithImm(e, true, dest, shapeToReg(e, accShape, SCRATCH_REG), rhs.imm); // both imm
        }
        return;
    }

    // RSUB: rhs - acc
    if(!rhs.isImm) {
        if(!accShape.isImm) e.emit(ArmV6M::subs(R((uint16_t)dest), R((uint16_t)rhs.reg), R((uint16_t)accShape.reg)));
        // acc imm (k), rhs reg (n): dest = n - k — an ordinary register-
        // minus-immediate, the ADD/SUB row's own fold.
        else addOrSubWithImm(e, true, dest, rhs.reg, accShape.imm);
    } else if(!accShape.isImm) {
        emitRsubImmAsLeft(e, dest, rhs.imm, accShape.reg);
    } else {
        uint32_t n = shapeToReg(e, accShape, SCRATCH_REG);
        emitRsubImmAsLeft(e, dest, rhs.imm, n); // both imm
    }
}

uint16_t shiftOpImm(Op op, uint32_t d, uint32_t m, int32_t amount) {
    if(op == Op::SHL) return ArmV6M::lsls(R((uint16_t)d), R((uint16_t)m), ArmV6M::Imm<5>((uint16_t)amount));
    if(op == Op::SHR) return ArmV6M::lsrs(R((uint16_t)d), R((uint16_t)m), ArmV6M::Imm<5>((uint16_t)amount));
    return ArmV6M::asrs(R((uint16_t)d), R((uint16_t)m), ArmV6M::Imm<5>((uint16_t)amount));
}

uint16_t twoOpInPlaceNative(Op op, uint32_t dn, uint32_t m) {
    switch(op) {
        case Op::AND: return ArmV6M::ands(R((uint16_t)dn), R((uint16_t)m));
        case Op::OR:  return ArmV6M::orrs(R((uint16_t)dn), R((uint16_t)m));
        case Op::XOR: return ArmV6M::eors(R((uint16_t)dn), R((uint16_t)m));
        case Op::MUL: return ArmV6M::muls(R((uint16_t)dn), R((uint16_t)m));
        case Op::SHL: return ArmV6M::lsls(R((uint16_t)dn), R((uint16_t)m));
        case Op::SHR: return ArmV6M::lsrs(R((uint16_t)dn), R((uint16_t)m));
        case Op::ASR: return ArmV6M::asrs(R((uint16_t)dn), R((uint16_t)m));
        default: assert(false); return 0; // GCOV_EXCL_LINE
    }
}

} // namespace

void emitBinaryOp(
    Emitter &e, Op op, Combo combo,
    const Shape &accShape, const Shape *operand, uint32_t dest)
{
    BinOpKind kind = classifyBinOp(op, combo);

    if(kind == BinOpKind::TwoOpInPlace) {
        assert(operand != nullptr); // GCOV_EXCL_LINE — PEEK_PEEK here not implemented, binops.ts's own gap
        // Never folds — materialize acc into ACC_REG specifically,
        // unconditionally, even when accShape is already some other
        // register: the native 2-op-in-place form's Rdn slot is both read
        // and written, and that other register can be a live variable's
        // own home.
        materializeShape(e, accShape, ACC_REG);
        uint32_t m = shapeToReg(e, *operand, SCRATCH_REG);
        e.emit(twoOpInPlaceNative(op, ACC_REG, m));
        if(dest != ACC_REG) e.emit(ArmV6M::mov(ArmV6M::AnyReg((uint16_t)dest), ArmV6M::AnyReg((uint16_t)ACC_REG)));
        return;
    }

    if(kind == BinOpKind::ShiftImm) {
        assert(operand != nullptr && operand->isImm); // GCOV_EXCL_LINE
        e.emit(shiftOpImm(op, dest, shapeToReg(e, accShape, SCRATCH_REG), operand->imm));
        return;
    }

    emitAddSubRsub(e, op, dest, accShape, operand);
}

} // namespace jitc
