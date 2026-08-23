#include "translate_proc.h"
#include "emitter.h"
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

#include <cassert>

namespace jitc {

using R = ArmV6M::LoReg;
using Cond = ArmV6M::Condition;

namespace {

/** Bounds the recursion LOOP/BR_TABLE nesting uses in place of an
 *  explicit block stack (blocks.h's own header has why) — on real
 *  hardware this is the check a finite call stack actually needs, unlike
 *  the prototype's own JS recursion limit, which sits far above any
 *  realistic procedure's nesting depth and was never the bound that
 *  mattered there. Exceeding it here folds into TranslateResult::
 *  overflowed (translate_proc.h's own header) rather than asserting —
 *  a too-deeply-nested procedure is exactly the kind of thing
 *  compile_proc_real.cpp's own caller already knows how to report as
 *  RESOURCE_ERROR, so there's no reason to invent a second channel. */
constexpr uint32_t MAX_BLOCK_NESTING = 32;

/** Placeholder BL sites for BR_TABLE N>2's own shared jump-table helper,
 *  collected across the whole procedure — mirrors unaryops.h's own fixed-
 *  capacity kMaxUnaryHelperSites reasoning: a real target bounds
 *  procedure size tightly enough that this comfortably covers any
 *  realistic body. */
constexpr uint32_t kMaxBrTableHelperSites = 32;

/** Slot k's window register a peek at the instruction starting at byte
 *  offset afterPc resolves to, if that instruction is a STORE targeting a
 *  currently in-window slot — the one-token destination-fold trigger
 *  every producer/consumer below checks before falling back to ACC_REG.
 *  -1 (not std::optional/nullptr) is the "no fold" sentinel, mirroring
 *  translateProc.ts's `number | null`. Returns the fold's own register and
 *  the STORE's own `next`, so a caller that takes the fold can jump pc
 *  straight there instead of re-decoding to find it (afterNext is only
 *  meaningful when a fold is returned). */
struct FoldResult { int32_t reg; uint32_t afterNext; };
FoldResult peekStoreFold(const uint8_t *bytes, uint32_t bytesLen, uint32_t afterPc, uint32_t tos) {
    if(afterPc >= bytesLen) return {-1, 0};
    DecodedInstr d = decodeInstr(bytes, bytesLen, afterPc);
    if(d.instr.op == Op::STORE && inWindow(tos, d.instr.target)) return {(int32_t)physReg(d.instr.target), d.next};
    return {-1, 0};
}

/** Whether this instruction carries a meaningful `target` slot index —
 *  translateProc.ts's own `"target" in instr` discriminated-union check,
 *  restated for Instr's flat-struct shape (instr.h's own header has why
 *  that field is always present but not always meaningful). Needed by
 *  the last-argument-fold reference scan below. */
bool hasTargetField(const Instr &i) {
    return i.op == Op::LOAD || i.op == Op::STORE || i.combo == Combo::REG_ACC || i.combo == Combo::REG_REG;
}

/** Whether this procedure's own body needs lr protected before anything
 *  can clobber it — a nested CALL, blocks.h's own emitBrTableHelper
 *  (BR_TABLE N > 2 only), or unaryops.h's CLZ/REVBITS helpers, all reached
 *  by a local BL that clobbers real hardware lr directly. */
bool needsLRSave(const Proc &proc) {
    uint32_t pc = 0;
    while(pc < proc.bodyBytes) {
        DecodedInstr d = decodeInstr(proc.body, proc.bodyBytes, pc);
        Op op = d.instr.op;
        if(op == Op::CALL) return true;
        if(op == Op::BR_TABLE && d.instr.imm > 2) return true;
        if(op == Op::CLZ || op == Op::REVBITS) return true;
        pc = d.next;
    }
    return false;
}

/** All the per-procedure state translateBody's own recursive calls (one
 *  per open LOOP/BR_TABLE, mirroring translateProc.ts's own nesting)
 *  share — passed by reference rather than captured, since a plain
 *  recursive function needs no closure machinery to do that. */
struct Ctx {
    Emitter &e;
    Window &window;
    AccState &accState;
    const uint8_t *bytes;
    uint32_t bytesLen;
    uint32_t pc = 0;
    const uint32_t *calleeArgCounts;
    uint32_t calleeCount;
    uint32_t procIdx;
    bool savesLR;
    uint32_t initialSpilledCount;

    uint32_t brTableHelperSites[kMaxBrTableHelperSites];
    uint32_t brTableHelperSiteCount = 0;
    UnaryHelperSites unaryHelperSites;

    bool hasPendingComparisonCondition = false;
    Cond pendingComparisonCondition = Cond::EQ;

    bool nestingExceeded = false;
};

void returnSequence(Ctx &ctx) {
    // Unwind whatever this body spilled — nothing downstream reads
    // r4-r7 again, so there's nothing to reload for, only sp to
    // rebalance before the real-ABI return sequence runs.
    ctx.window.discardWindow(ctx.e);
    abiEmitReturn(ctx.e, ctx.savesLR, ctx.initialSpilledCount);
}

/** isa-core.md §4.5/§7.1/§7.2: a case/loop body may close via a bare
 *  RETURN/TRAP instead of BLOCK_END — blocks.h's closeCaseViaTerminator/
 *  closeLoopBodyViaTerminator own doc comments have the full story.
 *  Mirrors the BLOCK_END case's own closeBlockEnd/frame-mutate dispatch,
 *  just triggered by a terminator instead. Returns whether frame is
 *  still open (mirrors closeBlockEnd's own convention). */
bool closeFrameForTerminator(Ctx &ctx, Frame &frame) {
    if(frame.kind == FrameKind::Case) return closeCaseViaTerminator(ctx.e, ctx.window, ctx.accState, frame);
    closeLoopBodyViaTerminator(ctx.e, ctx.window, ctx.accState, frame);
    return false;
}

/** Block nesting is native recursion, not an explicit stack — one call
 *  per open LOOP/BR_TABLE, its own Frame held as a plain local
 *  (blocks.h's own header has why). frame is this level's own open
 *  block, nullptr at the top level; depth is checked against
 *  MAX_BLOCK_NESTING before recursing any further. */
void translateBody(Ctx &ctx, Frame *frame, uint32_t depth) {
    if(depth > MAX_BLOCK_NESTING) { ctx.nestingExceeded = true; return; }

    while(ctx.pc < ctx.bytesLen) {
        DecodedInstr decoded = decodeInstr(ctx.bytes, ctx.bytesLen, ctx.pc);
        const Instr &instr = decoded.instr;
        uint32_t afterInstr = decoded.next;

        switch(instr.op) {
            case Op::CALL: {
                assert(instr.calleeIndex < ctx.calleeCount); // GCOV_EXCL_LINE — malformed program, not a runtime condition
                uint32_t calleeArgCount = ctx.calleeArgCounts[instr.calleeIndex];
                uint32_t stackArgs = calleeArgCount > 0 ? calleeArgCount - 1 : 0;

                // acc is unconditionally clobbered by CALL.
                ctx.accState.flush(ctx.e, ACC_REG);

                spillForCall(ctx.e, ctx.window, stackArgs);
                fillCalleeArgs(ctx.e, stackArgs);
                abiEmitCall(ctx.e, ctx.procIdx, instr.calleeIndex);
                reloadAfterCall(ctx.e, ctx.window, ctx.window.tos - stackArgs);

                // The return value is now in acc — a fresh producer, same
                // as any other, so a following STORE still folds.
                ctx.accState.producer(Shape::ofReg(ACC_REG));
                ctx.pc = afterInstr;
                continue;
            }

            case Op::PUSH:
                ctx.window.pushValue(ctx.e, ctx.accState);
                ctx.pc = afterInstr;
                continue;

            case Op::POP:
                ctx.e.emit(ArmV6M::mov(ArmV6M::AnyReg(ACC_REG), ArmV6M::AnyReg(ctx.window.topReg()))); // materialize now — a bare POP can't safely stay PENDING
                ctx.accState.setClean(ACC_REG);
                ctx.window.finishPop(ctx.e); // must run after the read above — same register
                ctx.pc = afterInstr;
                continue;

            case Op::NEG: case Op::NOT: case Op::CLZ: case Op::REVBITS: {
                // No fold axis of its own — always flush first, exactly
                // like the general binary-op "no match" fallback.
                ctx.accState.flush(ctx.e, ACC_REG);
                FoldResult fold = peekStoreFold(ctx.bytes, ctx.bytesLen, afterInstr, ctx.window.tos);
                uint32_t dest = fold.reg >= 0 ? (uint32_t)fold.reg : ACC_REG;
                emitUnary(ctx.e, instr.op, dest, ctx.unaryHelperSites);
                ctx.accState.setClean(dest);
                ctx.pc = fold.reg >= 0 ? fold.afterNext : afterInstr;
                continue;
            }

            case Op::BLOCK_END: {
                assert(frame != nullptr); // GCOV_EXCL_LINE — BLOCK_END with no open block; malformed program
                bool hasLoopExitCondition = false;
                Cond loopExitCondition = Cond::EQ;
                if(frame->kind == FrameKind::LoopCond) {
                    // isa-core.md §7.2's own leniency — fall back to an
                    // explicit CMP #0 when nothing was fused, rather than
                    // requiring the preceding instruction to have been a
                    // comparison.
                    Cond trueCondition = ctx.hasPendingComparisonCondition ? ctx.pendingComparisonCondition : testAccNonzero(ctx.e, ctx.accState);
                    ctx.hasPendingComparisonCondition = false;
                    loopExitCondition = ArmV6M::inverse(trueCondition);
                    hasLoopExitCondition = true;
                } else {
                    assert(!ctx.hasPendingComparisonCondition); // GCOV_EXCL_LINE — comparison fused into nothing; malformed program
                }
                bool stillOpen = closeBlockEnd(ctx.e, ctx.window, ctx.accState, *frame,
                    hasLoopExitCondition, loopExitCondition, ctx.bytes, ctx.bytesLen, ctx.pc);
                ctx.pc = afterInstr;
                if(!stillOpen) return;
                continue; // frame already mutated in place by closeBlockEnd
            }

            case Op::LOOP: {
                ctx.pc = afterInstr;
                Frame inner = openLoop(ctx.e, ctx.window, ctx.accState);
                translateBody(ctx, &inner, depth + 1);
                if(ctx.nestingExceeded) return;
                continue;
            }

            case Op::BR_TABLE: {
                // N ≤ 2 (if/if-else): a boolean-shaped acc, branch-fusable
                // against whatever comparison (if any) immediately
                // preceded this. N > 2: acc is a genuine multi-way
                // selector — its actual value is what's needed, so
                // there's nothing to fuse and no testAccNonzero CMP to
                // pay for either.
                uint32_t n = (uint32_t)instr.imm;
                ctx.pc = afterInstr;
                if(n > 2) {
                    OpenedJump opened = openBrTableJump(ctx.e, ctx.window, n, ctx.accState);
                    assert(ctx.brTableHelperSiteCount < kMaxBrTableHelperSites); // GCOV_EXCL_LINE — a procedure with more BR_TABLE N>2 sites than this needs kMaxBrTableHelperSites raised
                    ctx.brTableHelperSites[ctx.brTableHelperSiteCount++] = opened.helperSite;
                    ctx.hasPendingComparisonCondition = false;
                    translateBody(ctx, &opened.frame, depth + 1);
                } else {
                    Cond trueCondition = ctx.hasPendingComparisonCondition ? ctx.pendingComparisonCondition : testAccNonzero(ctx.e, ctx.accState);
                    ctx.hasPendingComparisonCondition = false;
                    // afterInstr (case 0's own true start), not BR_TABLE's
                    // own position — emitGuardedBranch's span bound must
                    // measure case 0's own body, never the BR_TABLE opcode
                    // itself (which would recurse into it as if it were a
                    // *nested* construct one level further in).
                    Frame inner = openBrTable(ctx.e, ctx.window, n, trueCondition, ctx.bytes, ctx.bytesLen, afterInstr);
                    translateBody(ctx, &inner, depth + 1);
                }
                if(ctx.nestingExceeded) return;
                continue;
            }

            case Op::RETURN: {
                ctx.accState.flush(ctx.e, ACC_REG); // isa-core.md §7: the return value is whatever's in acc
                returnSequence(ctx);
                ctx.pc = afterInstr;
                // isa-core.md §7.1/§7.2: a case/loop body may be closed by
                // a bare terminator instead of BLOCK_END — this one just
                // served as that closer, so whatever frame is currently
                // open needs the same forward-branch bookkeeping BLOCK_END
                // would have triggered, then either keeps translating
                // sibling cases or unwinds this recursive call, exactly
                // like the BLOCK_END case above.
                if(frame != nullptr) {
                    bool stillOpen = closeFrameForTerminator(ctx, *frame);
                    if(!stillOpen) return;
                }
                continue;
            }

            case Op::TRAP: {
                // No real Report/error model this slice — sentinel-encode
                // the trap (high bit set, low bits the trap code) the same
                // way the QEMU test harness already expects.
                emitSynthesizeImm32(ctx.e, ACC_REG, 0x80000000u | (uint32_t)instr.imm);
                returnSequence(ctx);
                ctx.pc = afterInstr;
                if(frame != nullptr) { // same as RETURN, above
                    bool stillOpen = closeFrameForTerminator(ctx, *frame);
                    if(!stillOpen) return;
                }
                continue;
            }

            case Op::LOAD: {
                if(!inWindow(ctx.window.tos, instr.target)) {
                    ctx.e.emit(ArmV6M::ldrSp(R(ACC_REG), ArmV6M::Uoff<2, 8>((uint16_t)ctx.window.spillOffset(instr.target))));
                    ctx.accState.setClean(ACC_REG);
                    ctx.pc = afterInstr;
                    continue;
                }
                FoldResult fold = peekStoreFold(ctx.bytes, ctx.bytesLen, afterInstr, ctx.window.tos);
                ctx.accState.producer(Shape::ofReg(physReg(instr.target)));
                if(fold.reg >= 0) { ctx.accState.flush(ctx.e, (uint32_t)fold.reg); ctx.pc = fold.afterNext; continue; }
                ctx.pc = afterInstr;
                continue;
            }

            case Op::STORE:
                if(!inWindow(ctx.window.tos, instr.target)) {
                    ctx.accState.flush(ctx.e, ACC_REG);
                    ctx.e.emit(ArmV6M::strSp(R(ACC_REG), ArmV6M::Uoff<2, 8>((uint16_t)ctx.window.spillOffset(instr.target))));
                    ctx.pc = afterInstr;
                    continue;
                }
                ctx.accState.flush(ctx.e, physReg(instr.target));
                ctx.pc = afterInstr;
                continue;

            case Op::CONST: {
                FoldResult fold = peekStoreFold(ctx.bytes, ctx.bytesLen, afterInstr, ctx.window.tos);
                uint32_t target = fold.reg >= 0 ? (uint32_t)fold.reg : ACC_REG;
                if(fitsImm8(instr.imm) && fold.reg < 0) {
                    ctx.accState.producer(Shape::ofImm(instr.imm)); // stay pending — a later consumer may fold it
                    ctx.pc = afterInstr;
                    continue;
                }
                emitSynthesizeImm32(ctx.e, target, (uint32_t)instr.imm);
                ctx.accState.setClean(target);
                ctx.pc = fold.reg >= 0 ? fold.afterNext : afterInstr;
                continue;
            }

            // Every remaining op (arithmetic + comparison) carries a
            // combo — the addressing-mode dimension layers onto every one
            // of them uniformly, so they share one dispatch path here.
            default: {
                Combo combo = instr.combo;
                Shape operandStorage{};
                bool hasOperand = true;
                bool popAfter = false;

                if(combo == Combo::REG_ACC || combo == Combo::REG_REG) {
                    if(inWindow(ctx.window.tos, instr.target)) operandStorage = Shape::ofReg(physReg(instr.target));
                    else {
                        ctx.e.emit(ArmV6M::ldrSp(R(SCRATCH_REG), ArmV6M::Uoff<2, 8>((uint16_t)ctx.window.spillOffset(instr.target))));
                        operandStorage = Shape::ofReg(SCRATCH_REG);
                    }
                } else if(combo == Combo::IMM_ACC) {
                    operandStorage = Shape::ofImm(instr.imm);
                } else if(combo == Combo::POP_ACC) {
                    operandStorage = Shape::ofReg(ctx.window.topReg());
                    popAfter = true;
                } else {
                    hasOperand = false; // PEEK_PEEK
                }

                if(isComparisonOp(instr.op)) {
                    // Fuse only when the *very* next instruction is the
                    // one thing that can actually consume a bare CMP as a
                    // condition: a BR_TABLE selector (N ≤ 2), or this
                    // LOOP's own condition sub-block closing. Anything
                    // else means this comparison is used as an ordinary
                    // value (§16 item 8), so its 0/1 result has to be
                    // materialized instead.
                    bool hasLookahead = afterInstr < ctx.bytesLen;
                    DecodedInstr lookahead = hasLookahead ? decodeInstr(ctx.bytes, ctx.bytesLen, afterInstr) : DecodedInstr{};
                    bool fusesIntoBrTable = hasLookahead && lookahead.instr.op == Op::BR_TABLE && lookahead.instr.imm <= 2;
                    bool fusesIntoLoopExit = frame != nullptr && frame->kind == FrameKind::LoopCond && hasLookahead && lookahead.instr.op == Op::BLOCK_END;

                    if(fusesIntoBrTable || fusesIntoLoopExit) {
                        Cond trueCondition = emitComparison(ctx.e, ctx.accState, instr.op, hasOperand ? &operandStorage : nullptr);
                        if(popAfter) ctx.window.finishPop(ctx.e);
                        ctx.hasPendingComparisonCondition = true;
                        ctx.pendingComparisonCondition = trueCondition;
                        ctx.pc = afterInstr;
                        continue;
                    }

                    FoldResult fold = peekStoreFold(ctx.bytes, ctx.bytesLen, afterInstr, ctx.window.tos);
                    uint32_t dest = fold.reg >= 0 ? (uint32_t)fold.reg : ACC_REG;
                    materializeComparison(ctx.e, ctx.accState, instr.op, hasOperand ? &operandStorage : nullptr, dest);
                    if(popAfter) ctx.window.finishPop(ctx.e);
                    ctx.accState.setClean(dest);
                    ctx.pc = fold.reg >= 0 ? fold.afterNext : afterInstr;
                    continue;
                }

                bool clobbersAcc = (combo == Combo::REG_REG || combo == Combo::PEEK_PEEK);
                uint32_t dest;
                uint32_t afterFold = afterInstr;
                int32_t storeBackOffset = -1;

                if(combo == Combo::REG_REG) {
                    if(inWindow(ctx.window.tos, instr.target)) dest = physReg(instr.target);
                    else { dest = SCRATCH_REG; storeBackOffset = (int32_t)ctx.window.spillOffset(instr.target); }
                } else if(combo == Combo::PEEK_PEEK) {
                    dest = ctx.window.topReg();
                } else {
                    FoldResult fold = peekStoreFold(ctx.bytes, ctx.bytesLen, afterInstr, ctx.window.tos);
                    dest = fold.reg >= 0 ? (uint32_t)fold.reg : ACC_REG;
                    if(fold.reg >= 0) afterFold = fold.afterNext;
                }

                emitBinary(ctx.e, ctx.accState, instr.op, combo, hasOperand ? &operandStorage : nullptr, dest, clobbersAcc);
                if(storeBackOffset >= 0) ctx.e.emit(ArmV6M::strSp(R(dest), ArmV6M::Uoff<2, 8>((uint16_t)storeBackOffset)));
                if(popAfter) ctx.window.finishPop(ctx.e);
                ctx.pc = afterFold;
                continue;
            }
        }
    }

    assert(frame == nullptr); // GCOV_EXCL_LINE — procedure body ended with an open block; malformed program
}

} // namespace

TranslateResult translateProc(
    const Proc &proc,
    uint32_t procIdx,
    const uint32_t *calleeArgCounts, uint32_t calleeCount,
    uint16_t *outBuf, uint32_t outCapacityHalfwords)
{
    Emitter e(outBuf, outCapacityHalfwords);
    bool savesLR = needsLRSave(proc);
    uint32_t initialSpilledCount = proc.argCount > WINDOW_SIZE ? proc.argCount - WINDOW_SIZE : 0;
    Window window(proc.argCount, savesLR);
    AccState accState;

    Ctx ctx{e, window, accState, proc.body, proc.bodyBytes,
        0, calleeArgCounts, calleeCount, procIdx, savesLR, initialSpilledCount};

    // Prologue — the fixed dispatch-table prologue stub, plus push{lr} if
    // this procedure's own body needs it protected.
    abiEmitPrologue(e, savesLR);

    // §6 callee-side prologue: the last argument (if any) arrives in acc.
    // §16 items 13/14's own fold: rather than unconditionally flushing it
    // into phys(argCount-1) regardless of whether the body ever reads
    // that slot by index, leave it PENDING (a fresh producer) whenever
    // that's provably safe — proven by a whole-body reference count, not
    // a one-token lookahead (translateProc.ts's own header has the full
    // safety argument, re-derived here against window.ts's own rotation-
    // eviction invariant unchanged by the port).
    if(proc.argCount >= 1) {
        uint32_t lastArgSlot = proc.argCount - 1;
        uint32_t refCount = 0;
        uint32_t firstRefPc = 0;
        bool firstRefIsBodyStart = false;
        Op firstRefOp = Op::PUSH; // arbitrary — only meaningful when firstRefIsBodyStart
        uint32_t p = 0;
        while(p < proc.bodyBytes) {
            DecodedInstr d = decodeInstr(proc.body, proc.bodyBytes, p);
            if(hasTargetField(d.instr) && d.instr.target == lastArgSlot) {
                if(refCount == 0) { firstRefPc = p; firstRefIsBodyStart = (p == 0); firstRefOp = d.instr.op; }
                refCount++;
            }
            p = d.next;
        }
        (void)firstRefPc;

        if(refCount == 0) {
            accState.producer(Shape::ofReg(ACC_REG));
        } else if(refCount == 1 && firstRefIsBodyStart && firstRefOp == Op::LOAD) {
            accState.producer(Shape::ofReg(ACC_REG));
            ctx.pc = decodeInstr(proc.body, proc.bodyBytes, 0).next;
        } else {
            accState.flush(e, physReg(lastArgSlot));
        }
    }

    translateBody(ctx, nullptr, 0);

    // BR_TABLE N>2's shared helper (blocks.h's own header) — dead code
    // from a sequential-execution standpoint, reached only by the local
    // BLs openBrTableJump already placed; emitted once, here, only if
    // this body actually used it.
    if(ctx.brTableHelperSiteCount > 0) {
        uint32_t helperOffset = emitBrTableHelper(e);
        for(uint32_t i = 0; i < ctx.brTableHelperSiteCount; i++) e.patchBL(ctx.brTableHelperSites[i], helperOffset);
    }

    // unaryops.h's own CLZ/REVBITS software helpers — same "shared,
    // emitted once, only if actually used" shape as BR_TABLE N>2's above.
    if(ctx.unaryHelperSites.clzCount > 0) {
        uint32_t helperOffset = emitClzHelper(e);
        for(uint32_t i = 0; i < ctx.unaryHelperSites.clzCount; i++) e.patchBL(ctx.unaryHelperSites.clz[i], helperOffset);
    }
    if(ctx.unaryHelperSites.revbitsCount > 0) {
        uint32_t helperOffset = emitRevbitsHelper(e);
        for(uint32_t i = 0; i < ctx.unaryHelperSites.revbitsCount; i++) e.patchBL(ctx.unaryHelperSites.revbits[i], helperOffset);
    }

    return TranslateResult{e.halfwordCount(), e.overflowed() || ctx.nestingExceeded};
}

} // namespace jitc
