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
        return;
    }

    uint32_t skip = a.placeholderCondBranch(ArmV6M::inverse(condition));
    a.branchTo(label); // the long unconditional branch, chained onto label
    a.patchBranch(skip, skip + 4); // "not taken" (condition true) — fall through to the long branch right after
}

Frame openBrTable(Assembler &a, Window &window, AccState &accState, uint32_t n, Cond condition, bool fused, const uint8_t *bytes, uint32_t bytesLen, uint32_t pc)
{
    assert(n == 1 || n == 2); // GCOV_EXCL_LINE — only if/if-else are supported; N>2 goes through openBrTableJump

    Frame frame{};
    frame.kind = FrameKind::Case;
    frame.entryTos = window.tos;
    frame.remaining = n;
    frame.table.present = false;
    frame.fusedBoolean = fused;

    // guards exactly case[0]'s own body — n==1 (bare if) has no case[1]
    // to skip to, so the guard chains onto endFixupChain directly
    // (self-linking as its sole entry, resolved the moment this frame's
    // one case closes); n==2 chains onto nextCaseFixup instead, resolved
    // the moment case[0] closes below.
    if(n == 1)
    {
        emitGuardedBranch(a, frame.endFixupChain, condition, bytes, bytesLen, pc, 1);
    }
    else
    {
        emitGuardedBranch(a, frame.nextCaseFixup, condition, bytes, bytesLen, pc, 1);
    }
    // case[0] (about to be translated by the caller) runs exactly when the
    // fused comparison was false — no register holds that result (the
    // branch above only ever set CPU flags), so seed accState with the
    // known constant instead of leaving whatever it was before the
    // comparison ran.
    if(fused)
    {
        accState.producer(Shape::ofImm(0));
    }
    return frame;
}

Frame openBrTableJump(Assembler &a, Window &window, uint32_t n, AccState &accState)
{
    accState.flush(a, ACC_REG);
    a.materializeImm32(SCRATCH_REG, n);
    // brTableJumpHelper, index 6 (docs/design.md §11's reserved slots) —
    // BLX, not BX: lr needs to end up pointing at the table emitted right
    // after this, exactly as it would after a local BL.
    a.emit(ArmV6M::mov(ArmV6M::AnyReg(ENTRY_JUMP_REG), ArmV6M::AnyReg(HELPER_VEC_REG)));
    a.emit(ArmV6M::ldr(R(ENTRY_JUMP_REG), R(ENTRY_JUMP_REG), ArmV6M::Uoff<2, 5>(24)));
    a.emit(ArmV6M::blx(ArmV6M::AnyReg(ENTRY_JUMP_REG)));

    uint32_t base = a.pc(); // == lr, once the BLX above actually executes
    for(uint32_t i = 0; i <= n; i++)
    {
        a.emit(0); // n+1 slots, contiguous from base
    }
    a.patchRawHalfword(base, (uint16_t)(a.pc() - base)); // case 0 starts right here — no fixup needed

    Frame frame{};
    frame.kind = FrameKind::Case;
    frame.entryTos = window.tos;
    frame.remaining = n;
    frame.table = TableInfo{true, base, base + 2, base + 2 * n};
    // Never fused (BR_TABLE N>2 is a genuine multi-way value, not a
    // boolean) — and unlike openBrTable's case[0], nothing needs seeding
    // here either: the flush above leaves ACC_REG holding the real
    // selector value unchanged all the way through the jump, which is
    // already exactly what every case index equals.
    frame.fusedBoolean = false;

    return frame;
}

Frame openLoop(Assembler &a, Window &window, AccState &accState)
{
    accState.flushLive(a, ACC_REG);
    Frame frame{};
    frame.kind = FrameKind::LoopCond;
    frame.entryTos = window.tos;
    frame.loopStart = a.pc();
    return frame;
}

/** The forward-branch bookkeeping a case frame's own close always needs —
 *  nextCaseFixup/the jump table's own next slot resolving to "wherever
 *  this case's own translated code ends," and (once the *last* case
 *  closes) endFixupChain/the table's own end slot resolving to the
 *  construct's shared end — regardless of *how* this case's own code
 *  actually ends. emitSkipToEnd is the one thing that genuinely differs
 *  between the two callers: a case that falls off the end of its own body
 *  (closeBlockEnd) has to actively branch past its own sibling cases'
 *  code; a case that ends via its own RETURN/TRAP (closeCaseViaTerminator)
 *  has already left the procedure entirely by the time this runs. */
static bool resolveCaseClose(Assembler &a, Frame &frame, bool emitSkipToEnd)
{
    frame.remaining -= 1;
    if(frame.remaining > 0 && emitSkipToEnd)
    {
        a.branchTo(frame.endFixupChain);
    }
    if(frame.nextCaseFixup.chain != -1)
    {
        a.bind(frame.nextCaseFixup);
    }
    else if(frame.table.present && frame.table.nextFixupSlot != frame.table.endSlot)
    {
        a.flushPool();
        a.patchRawHalfword(frame.table.nextFixupSlot, (uint16_t)(a.pc() - frame.table.base));
        frame.table.nextFixupSlot += 2;
    }
    if(frame.remaining > 0)
    {
        return true; // stay on this frame — now translating the next case
    }

    a.bind(frame.endFixupChain);
    if(frame.table.present)
    {
        a.patchRawHalfword(frame.table.endSlot, (uint16_t)(a.pc() - frame.table.base));
    }
    return false;
}

bool closeCaseViaTerminator(Assembler &a, Window &window, AccState &accState, Frame &frame)
{
    assert(frame.kind == FrameKind::Case); // GCOV_EXCL_LINE — translate_proc.cpp's own caller already dispatches on kind
    window.tos = frame.entryTos;
    accState.setClean(ACC_REG);
    return resolveCaseClose(a, frame, false);
}

void closeLoopBodyViaTerminator(Assembler &a, Window &window, AccState &accState, Frame &frame)
{
    assert(frame.kind == FrameKind::LoopBody); // GCOV_EXCL_LINE — see closeCaseViaTerminator's own comment
    a.bind(frame.exitFixup);
    window.tos = frame.entryTos;
    accState.setClean(ACC_REG);
}

bool closeBlockEnd(Assembler &a, Window &window, AccState &accState, Frame &frame,
    bool hasLoopExitCondition, Cond loopExitCondition, bool fusedLoopExit,
    const uint8_t *bytes, uint32_t bytesLen, uint32_t pc)
{
    if(frame.kind == FrameKind::Case)
    {
        restoreWindow(a, window, frame.entryTos);
        accState.flushLive(a, ACC_REG);
        bool stillOpen = resolveCaseClose(a, frame, true);
        // case[1] (about to be translated, if this frame has one) runs
        // exactly when the fused comparison was true — mirrors
        // openBrTable's own case[0] seeding.
        if(stillOpen && frame.fusedBoolean)
        {
            accState.producer(Shape::ofImm(1));
        }
        return stillOpen;
    }

    if(frame.kind == FrameKind::LoopCond)
    {
        restoreWindow(a, window, frame.entryTos);
        assert(hasLoopExitCondition); // GCOV_EXCL_LINE — LOOP condition block closed with no fused comparison to branch on
        emitGuardedBranch(a, frame.exitFixup, loopExitCondition, bytes, bytesLen, pc + 1, 1);
        frame.kind = FrameKind::LoopBody;
        // entryTos/loopStart already carried over from the LoopCond frame.
        // The body about to be translated runs exactly when the fused
        // comparison was true — same seeding as openBrTable's case[1],
        // same reason (no register holds it; testAccNonzero's own
        // unfused path needs no seeding since it never replaced acc's
        // real value in the first place).
        if(fusedLoopExit)
        {
            accState.producer(Shape::ofImm(1));
        }
        return true;
    }

    // LoopBody: unconditional back-edge, then the earlier exit branch
    // resolves to right after it — both targets were knowable without
    // ever looking past this point. Nothing falls through past the
    // back-edge (it's unconditional), so bind()'s own pool flush here is
    // always safe: the exit branch is the only thing that ever reaches
    // this address.
    restoreWindow(a, window, frame.entryTos);
    accState.flushLive(a, ACC_REG);
    a.emit(ArmV6M::b(ArmV6M::Ioff<1, 11>((int16_t)((int32_t)frame.loopStart - (int32_t)(a.pc() + 4)))));
    a.bind(frame.exitFixup);
    return false;
}

// ── Comparison → branch fusion ──────────────────────────────────────────

static constexpr Cond DIRECT_CONDITION[10] = {
    Cond::EQ, Cond::NE, Cond::LT, Cond::LE, Cond::GT, Cond::GE, Cond::LO, Cond::LS, Cond::HI, Cond::HS,
}; // EQ, NE, LT_S, LE_S, GT_S, GE_S, LT_U, LE_U, GT_U, GE_U

static constexpr Cond MIRRORED_CONDITION[10] = {
    Cond::EQ, Cond::NE, Cond::GT, Cond::GE, Cond::LT, Cond::LE, Cond::HI, Cond::HS, Cond::LO, Cond::LS,
};

Cond emitComparison(Assembler &a, AccState &accState, Op op, const Shape *operand)
{
    assert(isComparisonOp(op)); // GCOV_EXCL_LINE — translate_proc.cpp's own caller already checked
    uint32_t idx = (uint32_t)op - (uint32_t)Op::EQ;
    Cond condition = DIRECT_CONDITION[idx];

    assert(operand != nullptr); // GCOV_EXCL_LINE — PEEK_PEEK comparison fusion not implemented, not exercised by this corpus

    Shape left = accState.peek();

    if(left.isImm && !operand->isImm && fitsImm8(left.imm))
    {
        a.emit(ArmV6M::cmp(R((uint16_t)operand->reg), ArmV6M::Imm<8>((uint16_t)left.imm)));
        return MIRRORED_CONDITION[idx];
    }

    if(left.isImm)
    {
        materializeShape(a, left, ACC_REG);
        left = Shape::ofReg(ACC_REG);
    }

    if(!operand->isImm)
    {
        a.emit(ArmV6M::cmp(R((uint16_t)left.reg), R((uint16_t)operand->reg)));
    }
    else if(fitsImm8(operand->imm))
    {
        a.emit(ArmV6M::cmp(R((uint16_t)left.reg), ArmV6M::Imm<8>((uint16_t)operand->imm)));
    }
    else
    {
        materializeShape(a, *operand, SCRATCH_REG);
        a.emit(ArmV6M::cmp(R((uint16_t)left.reg), R((uint16_t)SCRATCH_REG)));
    }
    return condition;
}

Cond testAccNonzero(Assembler &a, AccState &accState)
{
    accState.flush(a, ACC_REG);
    a.emit(ArmV6M::cmp(R(ACC_REG), ArmV6M::Imm<8>(0)));
    return Cond::NE;
}

void materializeComparison(Assembler &a, AccState &accState, Op op, const Shape *operand, uint32_t dest)
{
    Cond trueCondition = emitComparison(a, accState, op, operand);
    Label falseLabel;
    a.branchTo(falseLabel, ArmV6M::inverse(trueCondition));
    a.emit(ArmV6M::movs(R((uint16_t)dest), ArmV6M::Imm<8>(1)));
    Label endLabel;
    a.branchTo(endLabel);
    a.bind(falseLabel);
    a.emit(ArmV6M::movs(R((uint16_t)dest), ArmV6M::Imm<8>(0)));
    a.bind(endLabel);
}

} // namespace jitc
