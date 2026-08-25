#include "blocks.h"
#include "emitter.h"
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

uint32_t emitGuardedBranch(Emitter &e, Cond condition, const uint8_t *bytes, uint32_t bytesLen, uint32_t from, uint32_t blockCount, uint32_t pendingPoolBytes)
{
    if(maxSpanBytes(bytes, bytesLen, from, blockCount).bytes + pendingPoolBytes <= SAFE_COND_BRANCH_SPAN)
    {
        return e.placeholderCondBranch(condition);
    }

    uint32_t skip = e.placeholderCondBranch(ArmV6M::inverse(condition));
    uint32_t site = e.placeholderBranch();
    e.patchBranch(skip, skip + 4); // "not taken" (condition true) — fall through to the long branch right after
    return site;
}

Frame openBrTable(Emitter &e, Window &window, AccState &accState, uint32_t n, Cond condition, bool fused, const uint8_t *bytes, uint32_t bytesLen, uint32_t pc, uint32_t pendingPoolBytes)
{
    assert(n == 1 || n == 2); // GCOV_EXCL_LINE — only if/if-else are supported; N>2 goes through openBrTableJump

    Frame frame{};
    frame.kind = FrameKind::Case;
    frame.entryTos = window.tos;
    frame.remaining = n;
    frame.nextCaseFixup = -1;
    frame.table.present = false;
    frame.endFixupChain = -1;
    frame.fusedBoolean = fused;

    uint32_t site = emitGuardedBranch(e, condition, bytes, bytesLen, pc, 1, pendingPoolBytes); // guards exactly case[0]'s own body
    if(n == 1)
    {
        e.patchBranch(site, site); // no case[1] to skip to — sole chain entry, self-linked to terminate
        frame.endFixupChain = (int32_t)site;
    }
    else
    {
        frame.nextCaseFixup = (int32_t)site; // target is "start of case[1]", resolved the moment case[0] closes below
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

Frame openBrTableJump(Emitter &e, Window &window, uint32_t n, AccState &accState)
{
    accState.flush(e, ACC_REG);
    emitSynthesizeImm32(e, SCRATCH_REG, n);
    // brTableJumpHelper, index 6 (docs/design.md §11's reserved slots) —
    // BLX, not BX: lr needs to end up pointing at the table emitted right
    // after this, exactly as it would after a local BL.
    e.emit(ArmV6M::mov(ArmV6M::AnyReg(ENTRY_JUMP_REG), ArmV6M::AnyReg(HELPER_VEC_REG)));
    e.emit(ArmV6M::ldr(R(ENTRY_JUMP_REG), R(ENTRY_JUMP_REG), ArmV6M::Uoff<2, 5>(24)));
    e.emit(ArmV6M::blx(ArmV6M::AnyReg(ENTRY_JUMP_REG)));

    uint32_t base = e.pc(); // == lr, once the BLX above actually executes
    for(uint32_t i = 0; i <= n; i++)
    {
        e.emit(0); // n+1 slots, contiguous from base
    }
    e.patchLiteral(base, (uint16_t)(e.pc() - base)); // case 0 starts right here — no fixup needed

    Frame frame{};
    frame.kind = FrameKind::Case;
    frame.entryTos = window.tos;
    frame.remaining = n;
    frame.nextCaseFixup = -1;
    frame.table = TableInfo{true, base, base + 2, base + 2 * n};
    frame.endFixupChain = -1;
    // Never fused (BR_TABLE N>2 is a genuine multi-way value, not a
    // boolean) — and unlike openBrTable's case[0], nothing needs seeding
    // here either: the flush above leaves ACC_REG holding the real
    // selector value unchanged all the way through the jump, which is
    // already exactly what every case index equals.
    frame.fusedBoolean = false;

    return frame;
}

Frame openLoop(Emitter &e, Window &window, AccState &accState)
{
    accState.flushLive(e, ACC_REG);
    Frame frame{};
    frame.kind = FrameKind::LoopCond;
    frame.entryTos = window.tos;
    frame.loopStart = e.pc();
    frame.exitFixup = -1;
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
static bool resolveCaseClose(Emitter &e, Frame &frame, bool emitSkipToEnd)
{
    frame.remaining -= 1;
    if(frame.remaining > 0 && emitSkipToEnd)
    {
        uint32_t site = e.placeholderBranch();
        e.patchBranch(site, frame.endFixupChain == -1 ? site : (uint32_t)frame.endFixupChain);
        frame.endFixupChain = (int32_t)site;
    }
    if(frame.nextCaseFixup != -1)
    {
        e.patchBranch((uint32_t)frame.nextCaseFixup, e.pc());
        frame.nextCaseFixup = -1;
    }
    else if(frame.table.present && frame.table.nextFixupSlot != frame.table.endSlot)
    {
        e.patchLiteral(frame.table.nextFixupSlot, (uint16_t)(e.pc() - frame.table.base));
        frame.table.nextFixupSlot += 2;
    }
    if(frame.remaining > 0)
    {
        return true; // stay on this frame — now translating the next case
    }

    for(int32_t site = frame.endFixupChain; site != -1;)
    {
        uint32_t prevSite = e.readBranchTarget((uint32_t)site);
        e.patchBranch((uint32_t)site, e.pc());
        site = (prevSite == (uint32_t)site) ? -1 : (int32_t)prevSite;
    }
    if(frame.table.present)
    {
        e.patchLiteral(frame.table.endSlot, (uint16_t)(e.pc() - frame.table.base));
    }
    return false;
}

bool closeCaseViaTerminator(Emitter &e, Window &window, AccState &accState, Frame &frame)
{
    assert(frame.kind == FrameKind::Case); // GCOV_EXCL_LINE — translate_proc.cpp's own caller already dispatches on kind
    window.tos = frame.entryTos;
    accState.setClean(ACC_REG);
    return resolveCaseClose(e, frame, false);
}

void closeLoopBodyViaTerminator(Emitter &e, Window &window, AccState &accState, Frame &frame)
{
    assert(frame.kind == FrameKind::LoopBody); // GCOV_EXCL_LINE — see closeCaseViaTerminator's own comment
    e.patchBranch((uint32_t)frame.exitFixup, e.pc());
    window.tos = frame.entryTos;
    accState.setClean(ACC_REG);
}

bool closeBlockEnd(Emitter &e, Window &window, AccState &accState, Frame &frame,
    bool hasLoopExitCondition, Cond loopExitCondition, bool fusedLoopExit,
    const uint8_t *bytes, uint32_t bytesLen, uint32_t pc, uint32_t pendingPoolBytes)
{
    if(frame.kind == FrameKind::Case)
    {
        restoreWindow(e, window, frame.entryTos);
        accState.flushLive(e, ACC_REG);
        bool stillOpen = resolveCaseClose(e, frame, true);
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
        restoreWindow(e, window, frame.entryTos);
        assert(hasLoopExitCondition); // GCOV_EXCL_LINE — LOOP condition block closed with no fused comparison to branch on
        uint32_t exitFixup = emitGuardedBranch(e, loopExitCondition, bytes, bytesLen, pc + 1, 1, pendingPoolBytes);
        frame.kind = FrameKind::LoopBody;
        frame.exitFixup = (int32_t)exitFixup;
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
    // ever looking past this point.
    restoreWindow(e, window, frame.entryTos);
    accState.flushLive(e, ACC_REG);
    e.emit(ArmV6M::b(ArmV6M::Ioff<1, 11>((int16_t)((int32_t)frame.loopStart - (int32_t)(e.pc() + 4)))));
    e.patchBranch((uint32_t)frame.exitFixup, e.pc());
    return false;
}

// ── Comparison → branch fusion ──────────────────────────────────────────

static constexpr Cond DIRECT_CONDITION[10] = {
    Cond::EQ, Cond::NE, Cond::LT, Cond::LE, Cond::GT, Cond::GE, Cond::LO, Cond::LS, Cond::HI, Cond::HS,
}; // EQ, NE, LT_S, LE_S, GT_S, GE_S, LT_U, LE_U, GT_U, GE_U

static constexpr Cond MIRRORED_CONDITION[10] = {
    Cond::EQ, Cond::NE, Cond::GT, Cond::GE, Cond::LT, Cond::LE, Cond::HI, Cond::HS, Cond::LO, Cond::LS,
};

Cond emitComparison(Emitter &e, AccState &accState, Op op, const Shape *operand)
{
    assert(isComparisonOp(op)); // GCOV_EXCL_LINE — translate_proc.cpp's own caller already checked
    uint32_t idx = (uint32_t)op - (uint32_t)Op::EQ;
    Cond condition = DIRECT_CONDITION[idx];

    assert(operand != nullptr); // GCOV_EXCL_LINE — PEEK_PEEK comparison fusion not implemented, not exercised by this corpus

    Shape left = accState.peek();

    if(left.isImm && !operand->isImm && fitsImm8(left.imm))
    {
        e.emit(ArmV6M::cmp(R((uint16_t)operand->reg), ArmV6M::Imm<8>((uint16_t)left.imm)));
        return MIRRORED_CONDITION[idx];
    }

    if(left.isImm)
    {
        materializeShape(e, left, ACC_REG);
        left = Shape::ofReg(ACC_REG);
    }

    if(!operand->isImm)
    {
        e.emit(ArmV6M::cmp(R((uint16_t)left.reg), R((uint16_t)operand->reg)));
    }
    else if(fitsImm8(operand->imm))
    {
        e.emit(ArmV6M::cmp(R((uint16_t)left.reg), ArmV6M::Imm<8>((uint16_t)operand->imm)));
    }
    else
    {
        materializeShape(e, *operand, SCRATCH_REG);
        e.emit(ArmV6M::cmp(R((uint16_t)left.reg), R((uint16_t)SCRATCH_REG)));
    }
    return condition;
}

Cond testAccNonzero(Emitter &e, AccState &accState)
{
    accState.flush(e, ACC_REG);
    e.emit(ArmV6M::cmp(R(ACC_REG), ArmV6M::Imm<8>(0)));
    return Cond::NE;
}

void materializeComparison(Emitter &e, AccState &accState, Op op, const Shape *operand, uint32_t dest)
{
    Cond trueCondition = emitComparison(e, accState, op, operand);
    uint32_t falseSite = e.placeholderCondBranch(ArmV6M::inverse(trueCondition));
    e.emit(ArmV6M::movs(R((uint16_t)dest), ArmV6M::Imm<8>(1)));
    uint32_t endSite = e.placeholderBranch();
    e.patchBranch(falseSite, e.pc());
    e.emit(ArmV6M::movs(R((uint16_t)dest), ArmV6M::Imm<8>(0)));
    e.patchBranch(endSite, e.pc());
}

} // namespace jitc
