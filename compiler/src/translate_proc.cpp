#include "translate_proc.h"
#include "assembler.h"
#include "window.h"
#include "accstate.h"
#include "shape.h"
#include "binops.h"
#include "abi_strategy.h"
#include "imm_synth.h"
#include "registers.h"
#include "armv6.h"
#include "decode_instr.h"
#include "blocks.h"
#include "unaryops.h"

#include "runtime_internal.h"

#include <cassert>

namespace jitc
{

using R = ArmV6M::LoReg;
using Cond = ArmV6M::Condition;

static constexpr uint32_t TRANSLATE_BODY_STACK_MARGIN = 512;

struct FoldResult
{
    int32_t reg;
    uint32_t afterNext;

    inline auto redirectReg(uint32_t otherwise) const 
    {
        return this->reg >= 0 ? (uint32_t)this->reg : otherwise;
    }

    inline auto redirectAfterNext(uint32_t otherwise) const {
        return this->reg >= 0 ? this->afterNext : otherwise;
    }
};

static FoldResult peekStoreFold(const uint8_t *bytes, uint32_t bytesLen, uint32_t afterPc, uint32_t tos)
{
    if(afterPc >= bytesLen)
    {
        return {-1, 0};
    }
    DecodedInstr d = decodeInstr(bytes, bytesLen, afterPc);
    if(d.instr.op == Op::STORE && inWindow(tos, d.instr.target))
    {
        return {(int32_t)physReg(d.instr.target), d.next};
    }
    return {-1, 0};
}

static bool hasTargetField(const Instr &i)
{
    return i.op == Op::LOAD || i.op == Op::STORE || i.combo == Combo::REG_ACC || i.combo == Combo::REG_REG;
}

struct Ctx
{
    Window window;
    const uint8_t *bytes;
    uint32_t bytesLen;
    uint32_t pc = 0;
    uint32_t procIdx;
    bool savesLR;
    uint32_t initialSpilledCount;

    bool hasPendingComparisonCondition = false;
    Cond pendingComparisonCondition = Cond::EQ;

    AccState accState;
};

static void returnSequence(Ctx &ctx, Assembler& a)
{
    ctx.window.discardWindow(a);
    abiEmitReturn(a, ctx.savesLR, ctx.initialSpilledCount);
}

static ArmV6M::Condition handleComparisonEmission(Ctx &ctx, Assembler& a, const Instr &instr)
{
    Combo combo = instr.combo;

    if(combo == Combo::REG_ACC || combo == Combo::REG_REG)
    {
        if(inWindow(ctx.window.tos, instr.target))
        {
            return emitComparison(a, ctx.accState, instr.op, Shape::ofReg(physReg(instr.target)));
        }
        else
        {
            a.emit(ArmV6M::ldrSp(R(SCRATCH_REG), ArmV6M::Uoff<2, 8>((uint16_t)ctx.window.spillOffset(instr.target))));
            return emitComparison(a, ctx.accState, instr.op, Shape::ofReg(SCRATCH_REG));
        }
    }
    else if(combo == Combo::IMM_ACC)
    {
        return emitComparison(a, ctx.accState, instr.op, Shape::ofImm(instr.imm));
    }
    else 
    {
        const auto ret = emitComparison(a, ctx.accState, instr.op, Shape::ofReg(ctx.window.topReg()));
        
        if(combo == Combo::POP_ACC)
        {
            ctx.window.finishPop(a);
        }

        return ret;
    }
}

static uint32_t processNonControl(Ctx &ctx, const DecodedInstr &decoded, Assembler& a, const Runtime& r)
{
    const Instr &instr = decoded.instr;
    uint32_t afterInstr = decoded.next;

    switch(instr.op)
    {
        case Op::CALL:
        {           
            uint32_t calleeArgCount = r.slot(instr.calleeIndex).argCount();
            uint32_t stackArgs = calleeArgCount > 0 ? calleeArgCount - 1 : 0;

            // acc is unconditionally clobbered by CALL.
            ctx.accState.flush(a, ACC_REG);

            spillForCall(a, ctx.window, stackArgs);
            fillCalleeArgs(a, stackArgs);
            abiEmitCall(a, ctx.procIdx, instr.calleeIndex);
            reloadAfterCall(a, ctx.window, ctx.window.tos - stackArgs);

            // The return value is now in acc — a fresh producer, same as
            // any other, so a following STORE still folds.
            ctx.accState.producer(Shape::ofReg(ACC_REG));
            return afterInstr;
        }
        case Op::PUSH:
            ctx.window.pushValue(a, ctx.accState);
            return afterInstr;
        case Op::POP:
            a.emit(ArmV6M::mov(ArmV6M::AnyReg(ACC_REG), ArmV6M::AnyReg(ctx.window.topReg()))); // materialize now — a bare POP can't safely stay PENDING
            ctx.accState.setClean(ACC_REG);
            ctx.window.finishPop(a); // must run after the read above — same register
            return afterInstr;
        case Op::NEG:
        case Op::NOT:
        case Op::CLZ:
        case Op::REVBITS:
        {
            // No fold axis of its own — always flush first, exactly like
            // the general binary-op "no match" fallback.
            ctx.accState.flush(a, ACC_REG);
            FoldResult fold = peekStoreFold(ctx.bytes, ctx.bytesLen, afterInstr, ctx.window.tos);
            uint32_t dest = fold.redirectReg(ACC_REG);
            
            emitUnary(a, instr.op, dest);
            ctx.accState.setClean(dest);
            return fold.redirectAfterNext(afterInstr);
        }
        case Op::LOAD:
        {
            if(!inWindow(ctx.window.tos, instr.target))
            {
                a.emit(ArmV6M::ldrSp(R(ACC_REG), ArmV6M::Uoff<2, 8>((uint16_t)ctx.window.spillOffset(instr.target))));
                ctx.accState.setClean(ACC_REG);
                return afterInstr;
            }

            FoldResult fold = peekStoreFold(ctx.bytes, ctx.bytesLen, afterInstr, ctx.window.tos);
            
            ctx.accState.producer(Shape::ofReg(physReg(instr.target)));

            if(fold.reg >= 0)
            {
                ctx.accState.flush(a, (uint32_t)fold.reg);
                return fold.afterNext;
            }

            return afterInstr;
        }

        case Op::STORE:
            if(!inWindow(ctx.window.tos, instr.target))
            {
                ctx.accState.flush(a, ACC_REG);
                a.emit(ArmV6M::strSp(R(ACC_REG), ArmV6M::Uoff<2, 8>((uint16_t)ctx.window.spillOffset(instr.target))));
                return afterInstr;
            }
            else
            {
                ctx.accState.flush(a, physReg(instr.target));
                return afterInstr;
            }

        case Op::CONST:
        {
            if(fitsImm8(instr.imm))
            {
                ctx.accState.producer(Shape::ofImm(instr.imm)); // stay pending — a later consumer may fold it
                return afterInstr;
            }
            else
            {
                FoldResult fold = peekStoreFold(ctx.bytes, ctx.bytesLen, afterInstr, ctx.window.tos);
                uint32_t target = fold.redirectReg(ACC_REG);
                a.materializeImm32(target, (uint32_t)instr.imm);
                ctx.accState.setClean(target);
                return fold.redirectAfterNext(afterInstr);
            }
        }
        case Op::ADD:
        case Op::SUB:
        case Op::RSUB:
        case Op::MUL:
        case Op::AND:
        case Op::OR:
        case Op::XOR:
        case Op::SHL:
        case Op::SHR:
        case Op::ASR:
        {
            Shape operandStorage{};
            switch (instr.combo)
            {
                case Combo::REG_ACC:
                case Combo::REG_REG:
                    if(inWindow(ctx.window.tos, instr.target))
                    {
                        operandStorage = Shape::ofReg(physReg(instr.target));
                    }
                    else
                    {
                        a.emit(ArmV6M::ldrSp(R(SCRATCH_REG), ArmV6M::Uoff<2, 8>((uint16_t)ctx.window.spillOffset(instr.target))));
                        operandStorage = Shape::ofReg(SCRATCH_REG);
                    }
                    break;
                case Combo::IMM_ACC:
                    operandStorage = Shape::ofImm(instr.imm);
                    break;
                case Combo::POP_ACC:
                case Combo::PEEK_PEEK:
                    operandStorage = Shape::ofReg(ctx.window.topReg());
                    break;
                default: assert(false);
            };

            if(instr.combo == Combo::REG_REG)
            {
                if(inWindow(ctx.window.tos, instr.target))
                {
                    emitBinaryOp(a, instr.op, instr.combo, ctx.accState.peek(), operandStorage, physReg(instr.target));
                }
                else
                {
                    emitBinaryOp(a, instr.op, instr.combo, ctx.accState.peek(), operandStorage, SCRATCH_REG);
                    a.emit(ArmV6M::strSp(R(SCRATCH_REG), ArmV6M::Uoff<2, 8>((uint16_t)ctx.window.spillOffset(instr.target))));
                }

                ctx.accState.poison();
                return afterInstr;
            }
            else if(instr.combo == Combo::PEEK_PEEK)
            {
                emitBinaryOp(a, instr.op, instr.combo, ctx.accState.peek(), operandStorage, ctx.window.topReg());
                ctx.accState.poison();
                return afterInstr;
            }
            else
            {
                FoldResult fold = peekStoreFold(ctx.bytes, ctx.bytesLen, afterInstr, ctx.window.tos);

                const auto dest = fold.redirectReg(ACC_REG);
                emitBinaryOp(a, instr.op, instr.combo, ctx.accState.peek(), operandStorage, dest);
                ctx.accState.setClean(dest);

                if(instr.combo == Combo::POP_ACC)
                {
                    ctx.window.finishPop(a);
                }

                return fold.redirectAfterNext(afterInstr);
            }
        }
        case Op::EQ:
        case Op::NE:
        case Op::LT_S:
        case Op::LE_S:
        case Op::GT_S:
        case Op::GE_S:
        case Op::LT_U:
        case Op::LE_U:
        case Op::GT_U:
        case Op::GE_U:
        {
            FoldResult fold = peekStoreFold(ctx.bytes, ctx.bytesLen, afterInstr, ctx.window.tos);
            uint32_t dest = fold.redirectReg(ACC_REG);

            Cond trueCondition = handleComparisonEmission(ctx, a, instr);

            Label falseLabel;
            a.branchTo(falseLabel, ArmV6M::inverse(trueCondition));
            a.emit(ArmV6M::movs(R((uint16_t)dest), ArmV6M::Imm<8>(1)));
            Label endLabel;
            a.branchTo(endLabel);
            a.bind(falseLabel);
            a.emit(ArmV6M::movs(R((uint16_t)dest), ArmV6M::Imm<8>(0)));
            a.bind(endLabel);

            ctx.accState.setClean(dest);
            return fold.redirectAfterNext(afterInstr);
        }
        default: assert(false); return -1u;
    }
}

static uint32_t processNonControlAndConditionalFolding(Ctx &ctx, DecodedInstr &decoded, Assembler& a, const Runtime& r, bool isThisLoopCondBlock)
{
    const Instr &instr = decoded.instr;
    uint32_t afterInstr = decoded.next;

    if(isComparisonOp(decoded.instr.op))
    {
        bool hasLookahead = afterInstr < ctx.bytesLen;
        DecodedInstr lookahead = hasLookahead ? decodeInstr(ctx.bytes, ctx.bytesLen, afterInstr) : DecodedInstr{};
        bool fusesIntoBrTable = hasLookahead && lookahead.instr.op == Op::BR_TABLE && lookahead.instr.imm <= 2;
        bool fusesIntoLoopExit = isThisLoopCondBlock && hasLookahead && lookahead.instr.op == Op::BLOCK_END;

        if(fusesIntoBrTable || fusesIntoLoopExit)
        {
            ctx.pendingComparisonCondition = handleComparisonEmission(ctx, a, instr);
            ctx.hasPendingComparisonCondition = true;
            return afterInstr;
        }
    }

    return processNonControl(ctx, decoded, a, r);
}

static void translateLoop(Ctx &ctx, Assembler& a, const Runtime& r);
static void translateIfThen(Ctx &ctx, Assembler& a, const Runtime& r);
static void translateIfThenElse(Ctx &ctx, Assembler& a, const Runtime& r);
static void translateSwitch(Ctx &ctx, Assembler& a, const Runtime& r, uint32_t n);

static void processNonTerminators(Ctx &ctx, DecodedInstr &decoded, Assembler& a, const Runtime& r, bool isThisLoopCondBlock)
{
    const Instr &instr = decoded.instr;
    uint32_t afterInstr = decoded.next;

    switch(instr.op)
    {
        case Op::LOOP:
        {
            ctx.pc = afterInstr;
            translateLoop(ctx, a, r);
            return;
        }

        case Op::BR_TABLE:
        {
            ctx.pc = afterInstr;

            switch(instr.imm)
            {
                case 1: translateIfThen(ctx, a, r); return;
                case 2: translateIfThenElse(ctx, a, r); return;
                default: translateSwitch(ctx, a, r, instr.imm); return;
            }
        }

        default:
            ctx.pc = processNonControlAndConditionalFolding(ctx, decoded, a, r, isThisLoopCondBlock);
            return;
    }
}

static Instr processUntilTerminator(Ctx &ctx, Assembler& a, const Runtime& r, bool isThisLoopCondBlock)
{
    while(ctx.pc < ctx.bytesLen)
    {
        DecodedInstr decoded = decodeInstr(ctx.bytes, ctx.bytesLen, ctx.pc);
        const Instr &instr = decoded.instr;
        uint32_t afterInstr = decoded.next;

        if(isTerminator(instr.op))
        {
            // The one legitimate case a pending fusion may still be set
            // here: fusesIntoLoopExit deliberately leaves it set right up
            // to the loop-cond block's own closing BLOCK_END, for
            // translateLoop's own caller to consume immediately after this
            // returns. Anything else (RETURN/TRAP, or a BLOCK_END outside
            // a loop-cond block) reaching here with a pending fusion means
            // the comparison it came from fused into nothing real.
            assert(!ctx.hasPendingComparisonCondition ||
                (isThisLoopCondBlock && instr.op == Op::BLOCK_END)); // GCOV_EXCL_LINE — comparison fused into nothing; malformed program

            ctx.pc = afterInstr;
            return instr;
        }

        processNonTerminators(ctx, decoded, a, r, isThisLoopCondBlock);
    }

    assert(false);
    for(;;);
}

static void localJumpCleanup(Ctx &ctx, Assembler& a, uint32_t tos)
{
    ctx.accState.flushLive(a, ACC_REG);
    restoreWindow(a, ctx.window, tos);
}

static void globalJumpCleanup(Ctx &ctx, Assembler& a, uint32_t tos)
{
    ctx.accState.setClean(ACC_REG);
    ctx.window.tos = tos;
}

static void handleGlobalJump(Ctx &ctx, Assembler& a, Instr term, uint32_t tos)
{
    if(term.op == Op::RETURN)
    {
        ctx.accState.flush(a, ACC_REG);
    }
    else
    {
        assert(term.op == Op::TRAP);
        a.materializeImm32(ACC_REG, 0x80000000u | (uint32_t)term.imm);
    }

    returnSequence(ctx, a);
    globalJumpCleanup(ctx, a, tos);
}

static void translateIfThen(Ctx &ctx, Assembler& a, const Runtime& r)
{
    const auto entryTos = ctx.window.tos;
    const bool fused = ctx.hasPendingComparisonCondition;
    ctx.hasPendingComparisonCondition = false;

    Label end;

    const auto cond = fused ? ctx.pendingComparisonCondition : testAccNonzero(a, ctx.accState);

    emitGuardedBranch(a, end, cond, ctx.bytes, ctx.bytesLen, ctx.pc, 1);

    if(fused)
    {
        ctx.accState.producer(Shape::ofImm(0));
    }   
    
    const auto term = processUntilTerminator(ctx, a, r, false);
    if(term.op == Op::BLOCK_END)
    {
        localJumpCleanup(ctx, a, entryTos);
    }
    else
    {
        handleGlobalJump(ctx, a, term, entryTos);
    }

    // "end" is also reached directly from the guarded branch above, on the
    // edge that skips the body entirely -- that edge never ran the body,
    // so code from here on can't rely on whatever the body's own
    // processing left ctx.accState as. Match globalJumpCleanup's own
    // convention (a real value ends up in ACC_REG) rather than carrying
    // over a stale Poisoned/Pending state from a path that wasn't taken.
    ctx.accState.setClean(ACC_REG);

    a.bind(end);
}

static void translateIfThenElse(Ctx &ctx, Assembler& a, const Runtime& r)
{
    const auto entryTos = ctx.window.tos;
    const bool fused = ctx.hasPendingComparisonCondition;
    ctx.hasPendingComparisonCondition = false;

    Label end, otherwise;

    const auto cond = fused ? ctx.pendingComparisonCondition : testAccNonzero(a, ctx.accState);

    emitGuardedBranch(a, otherwise, cond, ctx.bytes, ctx.bytesLen, ctx.pc, 1);

    if(fused)
    {
        ctx.accState.producer(Shape::ofImm(0));
    }

    const auto term = processUntilTerminator(ctx, a, r, false);
    if(term.op == Op::BLOCK_END)
    {
        localJumpCleanup(ctx, a, entryTos);
        a.branchTo(end);
    }
    else
    {
        handleGlobalJump(ctx, a, term, entryTos);
    }

    a.flushPoolNoGuard();
    a.bind(otherwise);

    if(fused)
    {
        ctx.accState.producer(Shape::ofImm(1));
    }
    else
    {
        // "otherwise" is reached exactly when "then" wasn't taken, so acc
        // still holds whatever testAccNonzero's flush() put in ACC_REG
        // right before the branch -- not whatever "then"'s own
        // (not-taken-here) body left ctx.accState as.
        ctx.accState.setClean(ACC_REG);
    }

    const auto term2 = processUntilTerminator(ctx, a, r, false);
    if(term2.op == Op::BLOCK_END)
    {
        localJumpCleanup(ctx, a, entryTos);
    }
    else
    {
        handleGlobalJump(ctx, a, term2, entryTos);
    }

    // "end" merges two edges -- "then"'s branch (taken when "then" ran and
    // fell through its own BLOCK_END) and "otherwise"'s fallthrough (taken
    // when "otherwise" ran) -- each leaving ctx.accState as whatever its
    // own body did. Code after this construct can't rely on either one in
    // particular, so land on the same fixed convention every other merge
    // point in this file uses.
    ctx.accState.setClean(ACC_REG);

    if(end.chain != -1)
    {
        a.bind(end);
    }
}

static void translateSwitch(Ctx &ctx, Assembler& a, const Runtime& r, uint32_t n)
{
    // XXX stack check

    const auto entryTos = ctx.window.tos;
    assert(ctx.hasPendingComparisonCondition == false);

    ctx.accState.flush(a, ACC_REG);
    a.materializeImm32(SCRATCH_REG, n);
    a.emit(ArmV6M::mov(ArmV6M::AnyReg(ENTRY_JUMP_REG), ArmV6M::AnyReg(HELPER_VEC_REG)));
    a.emit(ArmV6M::ldr(R(ENTRY_JUMP_REG), R(ENTRY_JUMP_REG), ArmV6M::Uoff<2, 5>(HELPER_BR_TABLE_JUMP_OFFSET)));
    a.emit(ArmV6M::blx(ArmV6M::AnyReg(ENTRY_JUMP_REG)));

    uint32_t base = a.pc(); 
    for(uint32_t i = 0; i <= n; i++)
    {
        a.emit(0); 
    }

    a.flushPoolNoGuard();

    Label end;

    for(uint32_t i = 0; i < n; i++)
    {
        a.patchRawHalfword(base + i * 2, (uint16_t)(a.pc() - base));

        // Every case is an alternative continuation from the same dispatch
        // point (the flush() above the jump table), not a continuation of
        // the previous case -- reset here so case i doesn't inherit
        // whatever accState mutation case i-1's own (not-taken-here) body
        // left behind.
        ctx.accState.setClean(ACC_REG);

        const auto term = processUntilTerminator(ctx, a, r, false);
        if(term.op == Op::BLOCK_END)
        {
            localJumpCleanup(ctx, a, entryTos);
            if(i + 1 < n)
            {
                a.branchTo(end);
                a.flushPoolNoGuard();
            }
        }
        else
        {
            handleGlobalJump(ctx, a, term, entryTos);
            a.flushPoolNoGuard();
        }
    }

    a.patchRawHalfword(base + n * 2, (uint16_t)(a.pc() - base));

    // "end" merges however many of the n cases branched here (each
    // leaving ctx.accState as whatever its own body did) plus the
    // fallthrough from the last case -- code after the switch can't rely
    // on any one of those, so land on the same fixed convention every
    // other merge point in this file uses (globalJumpCleanup's
    // setClean(ACC_REG)) rather than whatever the textually-last case
    // happened to leave behind.
    ctx.accState.setClean(ACC_REG);

    if(end.chain != -1)
    {
        a.bind(end);
    }
}

static void translateLoop(Ctx &ctx, Assembler& a, const Runtime& r)
{
    // XXX stack check
    const auto entryTos = ctx.window.tos;

    ctx.accState.flushLive(a, ACC_REG);
    const auto start = a.pc();

    const auto condTerm = processUntilTerminator(ctx, a, r, true);
    assert(condTerm.op == Op::BLOCK_END);

    const bool fused = ctx.hasPendingComparisonCondition;
    ctx.hasPendingComparisonCondition = false;

    const auto cond = fused ? ctx.pendingComparisonCondition : testAccNonzero(a, ctx.accState);

    Label out;
    emitGuardedBranch(a, out, ArmV6M::inverse(cond), ctx.bytes, ctx.bytesLen, ctx.pc, 1);

    if(fused)
    {
        ctx.accState.producer(Shape::ofImm(1));
    }

    const auto bodyTerm = processUntilTerminator(ctx, a, r, false);
    if(bodyTerm.op == Op::BLOCK_END)
    {
        localJumpCleanup(ctx, a, entryTos);
        a.emit(ArmV6M::b(ArmV6M::Ioff<1, 11>((int16_t)((int32_t)start - (int32_t)(a.pc() + 4)))));
    }
    else
    {
        handleGlobalJump(ctx, a, bodyTerm, entryTos);
    }

    a.flushPoolNoGuard();

    // "out" is reached directly from the guarded branch above whenever the
    // loop condition was false -- the body above never ran on that edge,
    // so acc must reflect a fixed, well-defined state (matching
    // testAccNonzero's own flush target), not whatever the body's own
    // processing left ctx.accState as.
    ctx.accState.setClean(ACC_REG);

    a.bind(out);
}

static void translateBody(Ctx &ctx, Assembler& a, const Runtime& r)
{
    register uint32_t sp asm("sp");
    if(sp < TRANSLATE_BODY_STACK_MARGIN || sp - TRANSLATE_BODY_STACK_MARGIN < a.stackFloor())
    {
        a.fail();
        return;
    }

    while(ctx.pc < ctx.bytesLen)
    {
        DecodedInstr decoded = decodeInstr(ctx.bytes, ctx.bytesLen, ctx.pc);
        const Instr &instr = decoded.instr;
        uint32_t afterInstr = decoded.next;

        switch(instr.op)
        {
        case Op::BLOCK_END:
            assert(false); // GCOV_EXCL_LINE — BLOCK_END with no open block; malformed program
            return;
                
        case Op::RETURN:
        case Op::TRAP:
        {
            handleGlobalJump(ctx, a, instr, ctx.window.tos);
            ctx.pc = afterInstr;
            assert(ctx.pc == ctx.bytesLen);
            return;
        }
                
        default:
            processNonTerminators(ctx, decoded, a, r, false);
            continue;
        }
    }
}

uint32_t translateProc(
    const Proc &proc,
    uint32_t procIdx,
    Assembler &a,
    const Runtime& r)
{
    uint32_t initialSpilledCount = proc.argCount > WINDOW_SIZE ? proc.argCount - WINDOW_SIZE : 0;
    const auto savesLR = r.slot(procIdx).needsLRSave();

    Ctx ctx{Window{proc.argCount, savesLR}, proc.body, proc.bodyBytes,
        0, procIdx, savesLR, initialSpilledCount};

    abiEmitPrologue(a, savesLR);

    if(proc.argCount >= 1 && proc.bodyBytes)
    {
        const auto lastArgSlot = proc.argCount - 1;
        const auto first = decodeInstr(proc.body, proc.bodyBytes, 0);
        const auto firstIsLastArgRef = first.instr.op == Op::LOAD && first.instr.target == lastArgSlot;

        auto failed = false;
        for(auto p = firstIsLastArgRef ? first.next : 0 ; p < proc.bodyBytes;)
        {
            const auto d = decodeInstr(proc.body, proc.bodyBytes, p);

            if(hasTargetField(d.instr) && d.instr.target == lastArgSlot)
            {
                failed = true;
                break;
            }

            p = d.next;
        }

        if(failed)
        {
            ctx.accState.flush(a, physReg(lastArgSlot));
        }
        else
        {
            ctx.accState.producer(Shape::ofReg(ACC_REG));
            if(firstIsLastArgRef)
            {
                ctx.pc = first.next;
            }
        }
    }

    translateBody(ctx, a, r);

    return a.finalize();
}

} // namespace jitc
