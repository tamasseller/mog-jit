#include "blocks.h"
#include "window.h"
#include "accstate.h"
#include "decode_instr.h"
#include "imm_synth.h"
#include "registers.h"

#include <cassert>

namespace jitc
{

using R = ArmV6M::LoReg;
using Cond = ArmV6M::Condition;

uint32_t instrMaxBytes(const Instr &instr)
{
    if(instr.op == Op::CALL)
    {
        return CALL_MAX_BYTES;
    }
    if(instr.op == Op::BR_TABLE && instr.imm > 2)
    {
        return BR_TABLE_JUMP_OVERHEAD_BYTES + (uint32_t)(instr.imm + 1) * 2;
    }
    return ORDINARY_MAX_BYTES;
}

// Thumb's real conditional-branch reach is ±252 bytes; this stays well
// under that so maxSpanBytes's own deliberate looseness never has to be
// exactly right, only safely conservative.
static constexpr uint32_t SAFE_COND_BRANCH_SPAN = 240;

SpanResult maxSpanBytes(const uint8_t *bytes, uint32_t bytesLen, uint32_t from, uint32_t blockCount)
{
    uint32_t pc = from;
    uint32_t total = 0;
    for(uint32_t remaining = blockCount; remaining > 0; remaining--)
    {
        for(;;)
        {
            assert(pc < bytesLen); // GCOV_EXCL_LINE — ran off the end while bounding a branch span; malformed input
            DecodedInstr d = decodeInstr(bytes, bytesLen, pc);
            total += instrMaxBytes(d.instr);
            if(d.instr.op == Op::BR_TABLE)
            {
                SpanResult sub = maxSpanBytes(bytes, bytesLen, d.next, (uint32_t)d.instr.imm);
                total += sub.bytes;
                pc = sub.nextPc;
                continue;
            }
            if(d.instr.op == Op::LOOP)
            {
                SpanResult sub = maxSpanBytes(bytes, bytesLen, d.next, 2);
                total += sub.bytes;
                pc = sub.nextPc;
                continue;
            }
            pc = d.next;
            if(d.instr.op == Op::BLOCK_END || d.instr.op == Op::RETURN || d.instr.op == Op::TRAP)
            {
                break;
            }
        }
    }
    return {total, pc};
}

void emitGuardedBranch(Assembler &a, Label &label, Cond condition, const uint8_t *bytes, uint32_t bytesLen, uint32_t from, uint32_t blockCount)
{
    if(maxSpanBytes(bytes, bytesLen, from, blockCount).bytes + a.poolDebt() <= SAFE_COND_BRANCH_SPAN)
    {
        a.branchTo(label, condition);
    }
    else
    {
        // Both branches go through Label/bind() rather than a raw
        // placeholderCondBranch patched to skip + 4. That arithmetic was
        // wrong whenever the literal pool had anything pending: the
        // unconditional branchTo() below flushes the pool right after
        // itself (nothing falls through an unconditional branch, so that
        // is normally free real estate) -- but skip + 4 points at exactly
        // that spot, so the "condition not taken" edge jumped straight
        // into literal data and executed it. Found by fuzz/qemu_exec:
        // `cmp r0,#0; bcs .+4; b otherwise; nop; <4 bytes of pool>` with
        // the pool word 0x0019d156 decoding as a `bne` into nothing.
        //
        // bind() is the one flush-safe way to resolve a fixup to "wherever
        // we are now" (assembler.h), and it is what makes this correct by
        // construction instead of by arithmetic that has to stay in step
        // with the pool.
        Label fallThrough;
        a.branchTo(fallThrough, ArmV6M::inverse(condition));
        a.branchTo(label); // the long unconditional branch, chained onto label
        a.bind(fallThrough);
    }
}

static constexpr Cond DIRECT_CONDITION[10] = {
    Cond::EQ, Cond::NE, Cond::LT, Cond::LE, Cond::GT, Cond::GE, Cond::LO, Cond::LS, Cond::HI, Cond::HS,
}; // EQ, NE, LT_S, LE_S, GT_S, GE_S, LT_U, LE_U, GT_U, GE_U

static constexpr Cond MIRRORED_CONDITION[10] = {
    Cond::EQ, Cond::NE, Cond::GT, Cond::GE, Cond::LT, Cond::LE, Cond::HI, Cond::HS, Cond::LO, Cond::LS,
};

Cond emitComparison(Assembler &a, AccState &accState, Op op, const Shape &operand)
{
    assert(isComparisonOp(op)); // GCOV_EXCL_LINE — translate_proc.cpp's own caller already checked
    uint32_t idx = (uint32_t)op - (uint32_t)Op::EQ;
    Cond condition = DIRECT_CONDITION[idx];

    Shape left = accState.peek();

    if(left.isImm && !operand.isImm && fitsImm8(left.imm))
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
    else if(fitsImm8(operand.imm))
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

Cond testAccNonzero(Assembler &a, AccState &accState)
{
    // No fixed-register requirement here (unlike a CALL/RETURN boundary) —
    // every caller only consumes the returned Cond and poisons acc right
    // after, so read the comparison operand from wherever it already
    // lives instead of forcing a flush into ACC_REG first, same as
    // emitComparison above.
    uint32_t r = accState.peek().peek(a, SCRATCH_REG);
    a.emit(ArmV6M::cmp(R(r), ArmV6M::Imm<8>(0)));
    return Cond::NE;
}

} // namespace jitc
