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

// Re-derived from real `-fstack-usage` measurements (test/qemu's build,
// same flags as production), not a round guess: the sustained chain from
// one checkStackFloor call succeeding to the next one running one level
// deeper is processNonTerminators (104) + processUntilTerminator (56),
// both simultaneously live (the outer's frame stays reserved while it
// calls the inner, which stays reserved while it recurses back into the
// next level's processNonTerminators) + checkStackFloor's own frame (8) at
// that deeper call = 168. The transient calls a construct handler makes
// before reaching that recursive call (testAccNonzero=16,
// emitGuardedBranch=40, handleComparisonEmission=40) are all smaller and
// have already returned by the time the deeper call happens, so none of
// them add to this. a.fail()'s own path was previously assumed to need
// margin too ("fail()'s own unwind path") — checked directly and it
// doesn't: runtimeBail() (dispatch_abi.cpp) never walks back through
// frames, it restores sp from Runtime::savedSp via inline asm and jumps
// straight to the landing point, so there's nothing proportional to depth
// to budget for there. 168 + a 56-byte pad for future drift = 224.
// test/qemu/Makefile's stack-usage-check target asserts this constant
// stays above the tracked 104+56+8 total on every build, so a future
// change to any of those three frames can't silently invalidate this
// number the way TRANSLATOR_ENTRY_WORST_CASE_BYTES once did.
static constexpr uint32_t TRANSLATE_BODY_STACK_MARGIN = 224;

// The real recursion isn't translateBody calling itself — it's
// translateLoop/translateIfThen/translateIfThenElse/translateSwitch each
// calling processUntilTerminator -> processNonTerminators -> back into any
// of those four (translateBody is only ever the depth-0 entry, never
// re-entered). So this needs checking at the top of all five, not just
// translateBody, for a nested LOOP/BR_TABLE to ever re-verify live SP as it
// recurses. r.liveStackFloor() is read fresh on every call (never cached),
// so this is self-correcting by construction — the only thing that can go
// stale is the margin itself (see its own derivation comment above).
static bool checkStackFloor(Assembler &a, const Runtime &r)
{
    register uint32_t sp asm("sp");
    if(sp < TRANSLATE_BODY_STACK_MARGIN || sp - TRANSLATE_BODY_STACK_MARGIN < r.liveStackFloor())
    {
        a.fail();
        return false;
    }
    return true;
}

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

// spillOffset() grows with tos (bounded only by MAX_BODY_BYTES, not by
// Uoff<2,8>'s 1020-byte encodable ceiling) — gate it here rather than
// letting fmtImm8's unmasked OR corrupt the target instruction's register
// field.
static ArmV6M::Uoff<2, 8> spillImm(Assembler &a, uint32_t byteOffset)
{
    if(!ArmV6M::Uoff<2, 8>::isInRange(byteOffset))
    {
        a.fail();
    }
    return ArmV6M::Uoff<2, 8>((uint16_t)byteOffset);
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
            a.emit(ArmV6M::ldrSp(R(SCRATCH_REG), spillImm(a, ctx.window.spillOffset(instr.target))));
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
            // instr.calleeIndex is a wire-decoded value validateProgram is
            // supposed to have already bounds-checked upstream — but
            // nothing downstream of the wire bytes re-derives that
            // guarantee, and r.slot() itself applies no bound of its own
            // (runtime_internal.h), so an out-of-range index would
            // otherwise read a garbage argCount here and bake a garbage
            // dispatch-table offset into abiEmitCall below. Cheap enough
            // to check directly rather than trust the upstream contract.
            if(instr.calleeIndex >= r.procCount)
            {
                a.fail();
                return afterInstr;
            }

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
        {
            // negs/mvns have an independent source register field — read
            // the operand from wherever it already lives instead of
            // forcing a flush into ACC_REG first (unlike CLZ/REVBITS
            // below, which have no such freedom). A still-pending
            // immediate materializes straight into dest rather than a
            // scratch register — negs/mvns allow dest==src, so this
            // stays a single instruction instead of a materialize-then-
            // negate-elsewhere pair.
            FoldResult fold = peekStoreFold(ctx.bytes, ctx.bytesLen, afterInstr, ctx.window.tos);
            uint32_t dest = fold.redirectReg(ACC_REG);
            uint32_t src = ctx.accState.peek().peek(a, dest);

            emitUnary(a, instr.op, dest, src);
            ctx.accState.setClean(dest);
            return fold.redirectAfterNext(afterInstr);
        }
        case Op::CLZ:
        case Op::REVBITS:
        {
            // Both dispatch through a fixed helper-vector subroutine that
            // hardcodes ACC_REG as both argument and return register —
            // no fold axis of their own, always flush first.
            ctx.accState.flush(a, ACC_REG);
            FoldResult fold = peekStoreFold(ctx.bytes, ctx.bytesLen, afterInstr, ctx.window.tos);
            uint32_t dest = fold.redirectReg(ACC_REG);

            emitUnary(a, instr.op, dest, ACC_REG);
            ctx.accState.setClean(dest);
            return fold.redirectAfterNext(afterInstr);
        }
        case Op::LOAD:
        {
            if(!inWindow(ctx.window.tos, instr.target))
            {
                a.emit(ArmV6M::ldrSp(R(ACC_REG), spillImm(a, ctx.window.spillOffset(instr.target))));
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
                // strSp's own register field is independent of ACC_REG,
                // and nothing downstream reads acc after this store — read
                // the value from wherever it already lives instead of
                // forcing a flush into ACC_REG first.
                uint32_t r = ctx.accState.peek().peek(a, SCRATCH_REG);
                a.emit(ArmV6M::strSp(R(r), spillImm(a, ctx.window.spillOffset(instr.target))));
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
                        a.emit(ArmV6M::ldrSp(R(SCRATCH_REG), spillImm(a, ctx.window.spillOffset(instr.target))));
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
                    a.emit(ArmV6M::strSp(R(SCRATCH_REG), spillImm(a, ctx.window.spillOffset(instr.target))));
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
        // Must match processNonTerminators's own BR_TABLE dispatch exactly
        // (translateIfThen/translateIfThenElse are the only two that
        // consume a pending fused comparison) — N == 0 or negative (an
        // overlong LEB128) falls through to translateSwitch instead, which
        // asserts none is pending; disagreeing here would leak the fusion
        // into whatever construct follows instead of tripping that assert.
        bool fusesIntoBrTable = hasLookahead && lookahead.instr.op == Op::BR_TABLE
            && (lookahead.instr.imm == 1 || lookahead.instr.imm == 2);
        bool fusesIntoLoopExit = isThisLoopCondBlock && hasLookahead && lookahead.instr.op == Op::BLOCK_END;

        if(fusesIntoBrTable || fusesIntoLoopExit)
        {
            ctx.pendingComparisonCondition = handleComparisonEmission(ctx, a, instr);
            ctx.hasPendingComparisonCondition = true;
            // isa-core.md §8.7: this comparison feeds a split (a fused
            // guarded branch) -- nothing downstream may read accState
            // until an arm/edge re-establishes it, whether via the
            // entering-direction seeds below or a merge point's own
            // poison(). Safe unconditionally: nothing between here and
            // whichever construct consumes hasPendingComparisonCondition
            // ever calls peek()/flush() on accState directly.
            ctx.accState.poison();
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

    // Ran off bytesLen without finding this block's own close — same
    // malformed/truncated-bytecode case decode_instr.h's own asserts
    // already leave out of scope (a translator-input bug, never a
    // legitimate runtime condition, so this doesn't get its own real
    // check either). assert(false) compiles out under -DNDEBUG same as
    // those; the for(;;) beneath it is unconditional either way, purely
    // to satisfy this non-void function's return requirement — reaching
    // it in a release build hangs rather than returning garbage.
    assert(false); // GCOV_EXCL_LINE
    for(;;);
}

static void localJumpCleanup(Ctx &ctx, Assembler& a, uint32_t tos)
{
    ctx.accState.flushLive(a, ACC_REG);
    restoreWindow(a, ctx.window, tos);
}

static void globalJumpCleanup(Ctx &ctx, Assembler& a, uint32_t tos)
{
    // Every caller either immediately re-poisons at its own construct's
    // next case/merge point, or (translateBody's own top-level RETURN/TRAP)
    // is the last thing translation does at all -- this write is provably
    // never read, but poison() rather than setClean(ACC_REG) keeps that
    // true by construction instead of by accident, matching isa-core.md
    // §8.7 (a terminator's own edge doesn't survive to feed anything else).
    ctx.accState.poison();
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
    if(!checkStackFloor(a, r))
    {
        return;
    }

    const auto entryTos = ctx.window.tos;
    const bool fused = ctx.hasPendingComparisonCondition;
    ctx.hasPendingComparisonCondition = false;

    Label skip;

    // What acc holds on the skip edge -- isa-core.md §4.5's implicit
    // default (acc >= N runs no case at all) leaves acc untouched, so that
    // edge re-establishes it for the merge below exactly as a case body
    // does, and validateProgram's own AND-across-cases model (§8.7)
    // therefore lets a RETURN downstream read it. Captured before the CMP:
    // testAccNonzero may peek a pending immediate into SCRATCH_REG, and
    // the fixup below wants the original description, not that scratch
    // copy. Nothing between here and the fixup can invalidate it -- the
    // fixup is only ever reached via the skip branch, i.e. with the body's
    // own code (window restore included) unexecuted. Fused, acc was never
    // materialized at all, but the skip edge is by construction the
    // "comparison true" one, so acc is 1 there (§7.3's complementary
    // comparison).
    const Shape skipEdgeAcc = fused ? Shape::ofImm(1) : ctx.accState.peek();

    const auto cond = fused ? ctx.pendingComparisonCondition : testAccNonzero(a, ctx.accState);

    emitGuardedBranch(a, skip, cond, ctx.bytes, ctx.bytesLen, ctx.pc, 1);

    if(fused)
    {
        ctx.accState.producer(Shape::ofImm(0));
    }   
    
    const auto term = processUntilTerminator(ctx, a, r, false);

    if(term.op != Op::BLOCK_END)
    {
        handleGlobalJump(ctx, a, term, entryTos);
        // The body left through its own terminator, so the skip edge is
        // the only one that reaches here -- no merge to reconcile, acc is
        // just whatever that one edge carries.
        a.bind(skip);
        ctx.accState.producer(skipEdgeAcc);
        return;
    }

    localJumpCleanup(ctx, a, entryTos);

    if(!ctx.accState.live())
    {
        // The body's edge arrives dead, so nothing downstream may read acc
        // regardless of which edge ran (isa-core.md §8.7) -- no fixup
        // needed, and the skip edge's own value goes unused.
        a.bind(skip);
        return;
    }

    // Both edges reach the merge live, but with the body's value in
    // ACC_REG (localJumpCleanup's flushLive) and the skip edge's still
    // wherever it started -- so the skip edge gets a fixup stub of its
    // own, placed after the body where only its branch can reach it.
    Label end;
    a.branchTo(end);
    a.bind(skip);
    skipEdgeAcc.materialize(a, ACC_REG);
    a.bind(end);
    ctx.accState.setClean(ACC_REG);
}

static void translateIfThenElse(Ctx &ctx, Assembler& a, const Runtime& r)
{
    if(!checkStackFloor(a, r))
    {
        return;
    }

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

    bool mergeLive = true; // vacuously, until an edge that actually reaches it says otherwise

    const auto term = processUntilTerminator(ctx, a, r, false);
    if(term.op == Op::BLOCK_END)
    {
        localJumpCleanup(ctx, a, entryTos);
        mergeLive = mergeLive && ctx.accState.live();
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
        // "otherwise" is reached exactly when "then" wasn't taken. Even
        // though testAccNonzero's own flush() really did put a value in
        // ACC_REG right before the branch, isa-core.md §8.7 still treats
        // this edge as a split successor -- nothing downstream may assume
        // that value is still meaningful without its own fresh producer.
        ctx.accState.poison();
    }

    const auto term2 = processUntilTerminator(ctx, a, r, false);
    if(term2.op == Op::BLOCK_END)
    {
        localJumpCleanup(ctx, a, entryTos);
        mergeLive = mergeLive && ctx.accState.live();
    }
    else
    {
        handleGlobalJump(ctx, a, term2, entryTos);
    }

    // "end" merges two edges -- "then"'s branch (taken when "then" ran and
    // fell through its own BLOCK_END) and "otherwise"'s fallthrough (taken
    // when "otherwise" ran); the two cases exhaust this form's successors,
    // since N == 2's implicit default is folded into "otherwise" by the
    // single conditional branch above. isa-core.md §8.7: live past the
    // merge only if every edge that actually reaches it re-established a
    // value -- localJumpCleanup's own flushLive already put each such
    // edge's value in ACC_REG, so agreement is all that's left to check.
    // An arm that left through a terminator never reaches here at all, and
    // if neither did, this is dead code and the answer doesn't matter.
    if(mergeLive)
    {
        ctx.accState.setClean(ACC_REG);
    }
    else
    {
        ctx.accState.poison();
    }

    if(end.chain != -1)
    {
        a.bind(end);
    }
}

static void translateSwitch(Ctx &ctx, Assembler& a, const Runtime& r, uint32_t n)
{
    if(!checkStackFloor(a, r))
    {
        return;
    }

    const auto entryTos = ctx.window.tos;
    assert(ctx.hasPendingComparisonCondition == false);

    ctx.accState.flush(a, ACC_REG);
    a.materializeImm32(SCRATCH_REG, n);

    // The dispatch (mov/ldr/blx, 6 bytes) and the n+1 raw table halfwords
    // that follow must sit contiguous — the helper jumps by indexing
    // directly off where the blx itself lands, so nothing may flush in
    // between. n is known here, so fold the whole span's own length into
    // the reach check up front instead of guarding unconditionally: a
    // switch big enough for this to actually matter is rare in practice.
    uint32_t tableBytes = 6 + (n + 1) * 2;
    a.ensurePoolRoom(0, tableBytes);

    uint32_t base;
    {
        Assembler::AtomicScope atomic(a);
        a.emit(ArmV6M::mov(ArmV6M::AnyReg(ENTRY_JUMP_REG), ArmV6M::AnyReg(HELPER_VEC_REG)));
        a.emit(ArmV6M::ldr(R(ENTRY_JUMP_REG), R(ENTRY_JUMP_REG), ArmV6M::Uoff<2, 5>(HELPER_BR_TABLE_JUMP_OFFSET)));
        a.emit(ArmV6M::blx(ArmV6M::AnyReg(ENTRY_JUMP_REG)));

        base = a.pc();
        for(uint32_t i = 0; i <= n; i++)
        {
            a.emit(0);
        }
    }

    a.flushPoolNoGuard();

    Label end;

    // Vacuously true: the implicit default (isa-core.md §4.5) always
    // reaches the merge and always arrives live -- the flush() above put
    // the dispatch value in ACC_REG, and the helper's own clamped index
    // register is deliberately never acc (docs/design.md §12), so that
    // edge leaves it untouched. Only the cases can spoil it.
    bool mergeLive = true;

    for(uint32_t i = 0; i < n; i++)
    {
        a.patchRawHalfword(base + i * 2, (uint16_t)(a.pc() - base));

        // Every case is a split successor of the same dispatch point (the
        // flush() above the jump table), not a continuation of the
        // previous case -- reset here so case i doesn't inherit whatever
        // accState mutation case i-1's own (not-taken-here) body left
        // behind. isa-core.md §8.7: a split clobbers acc unconditionally,
        // so this is poison() even though the dispatch value is still
        // physically sitting in ACC_REG -- nothing may read it without a
        // fresh producer of its own.
        ctx.accState.poison();

        const auto term = processUntilTerminator(ctx, a, r, false);
        if(term.op == Op::BLOCK_END)
        {
            localJumpCleanup(ctx, a, entryTos);
            mergeLive = mergeLive && ctx.accState.live();
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

    // "end" merges however many of the n cases branched here plus the
    // implicit default's own table slot, just patched above. isa-core.md
    // §8.7: live past the merge only if every edge that reaches it
    // re-established a value -- localJumpCleanup's own flushLive put each
    // BLOCK_END-closing case's value in ACC_REG and the default edge left
    // the dispatch value there, so agreement is all that's left to check.
    if(mergeLive)
    {
        ctx.accState.setClean(ACC_REG);
    }
    else
    {
        ctx.accState.poison();
    }

    if(end.chain != -1)
    {
        a.bind(end);
    }
}

static void translateLoop(Ctx &ctx, Assembler& a, const Runtime& r)
{
    if(!checkStackFloor(a, r))
    {
        return;
    }

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
        int32_t delta = (int32_t)start - (int32_t)(a.pc() + 4);
        if(!ArmV6M::Ioff<1, 11>::isInRange(delta))
        {
            a.fail();
            return;
        }
        a.emit(ArmV6M::b(ArmV6M::Ioff<1, 11>((int16_t)delta)));
    }
    else
    {
        handleGlobalJump(ctx, a, bodyTerm, entryTos);
    }

    a.flushPoolNoGuard();

    // "out" is reached directly from the guarded branch above whenever the
    // loop condition was false -- the body above never ran on that edge.
    // isa-core.md §8.7: this is a split successor of the condition's own
    // branch decision, so it starts dead regardless of what the condition
    // itself (or testAccNonzero's own flush) left in ACC_REG.
    ctx.accState.poison();

    a.bind(out);
}

static void translateBody(Ctx &ctx, Assembler& a, const Runtime& r)
{
    if(!checkStackFloor(a, r))
    {
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

    // isa-core.md §4.6: the last argument arrives in acc, not at
    // physReg(argCount-1) — that slot's own physical register holds
    // whatever the caller's shuffle last left there. Flush it immediately
    // so window.topReg()/physReg(argCount-1) are trustworthy from the
    // first instruction onward, same as every other in-window slot.
    if(proc.argCount >= 1)
    {
        ctx.accState.flush(a, physReg(proc.argCount - 1));
    }

    translateBody(ctx, a, r);

    return a.finalize();
}

} // namespace jitc
