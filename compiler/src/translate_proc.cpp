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

#include <cassert>

namespace jitc {

using R = ArmV6M::LoReg;

namespace {

/** Slot k's window register a peek at body[pc+1] resolves to, if that next
 *  instruction is a STORE targeting a currently in-window slot — the
 *  one-token destination-fold trigger every producer/consumer below
 *  checks before falling back to ACC_REG. -1 (not std::optional/nullptr)
 *  is the "no fold" sentinel, mirroring translateProc.ts's `number | null`. */
int32_t peekStoreFold(const Instr *body, uint32_t bodyCount, uint32_t pc, uint32_t tos) {
    if(pc + 1 >= bodyCount) return -1;
    const Instr &next = body[pc + 1];
    if(next.op == Op::STORE && inWindow(tos, next.target)) return (int32_t)physReg(next.target);
    return -1;
}

/** Whether this procedure's own body needs lr protected before a nested
 *  CALL can clobber it — translateProc.ts's needsLRSave, minus its
 *  BR_TABLE clause (not in this slice's Op set at all). Feeds both
 *  abiEmitPrologue/abiEmitReturn's dispatch choice and Window's own
 *  spillOffset()/discardWindow() adjustment. */
bool needsLRSave(const Proc &proc) {
    for(uint32_t i = 0; i < proc.bodyCount; i++) if(proc.body[i].op == Op::CALL) return true;
    return false;
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
    const Instr *body = proc.body;
    uint32_t bodyCount = proc.bodyCount;

    // Prologue — the fixed dispatch-table prologue stub, plus push{lr} if
    // this procedure's own body makes a nested CALL.
    abiEmitPrologue(e, savesLR);

    // Callee-side prologue: the last argument (if any) arrives in acc and
    // must land at phys(argCount-1) before anything reads it.
    if(proc.argCount >= 1) accState.flush(e, physReg(proc.argCount - 1));

    uint32_t pc = 0;
    while(pc < bodyCount) {
        const Instr &instr = body[pc];

        switch(instr.op) {
            case Op::CALL: {
                assert(instr.calleeIndex < calleeCount); // GCOV_EXCL_LINE — malformed program, not a runtime condition
                uint32_t calleeArgCount = calleeArgCounts[instr.calleeIndex];
                uint32_t stackArgs = calleeArgCount > 0 ? calleeArgCount - 1 : 0;

                // acc is unconditionally clobbered by CALL.
                accState.flush(e, ACC_REG);

                spillForCall(e, window, stackArgs);
                fillCalleeArgs(e, stackArgs);
                abiEmitCall(e, procIdx, instr.calleeIndex);
                reloadAfterCall(e, window, window.tos - stackArgs);

                // The return value is now in acc — a fresh producer, same
                // as any other, so a following STORE still folds.
                accState.producer(Shape::ofReg(ACC_REG));
                pc++;
                continue;
            }

            case Op::RETURN: {
                accState.flush(e, ACC_REG); // the return value is whatever's in acc
                window.discardWindow(e);
                abiEmitReturn(e, savesLR, initialSpilledCount);
                pc++;
                continue;
            }

            case Op::TRAP: {
                // No real Report/error model this slice — sentinel-encode
                // the trap (high bit set, low bits the trap code) the same
                // way the QEMU test harness already expects.
                emitSynthesizeImm32(e, ACC_REG, 0x80000000u | (uint32_t)instr.imm);
                window.discardWindow(e);
                abiEmitReturn(e, savesLR, initialSpilledCount);
                pc++;
                continue;
            }

            case Op::PUSH:
                window.pushValue(e, accState);
                pc++;
                continue;

            case Op::POP:
                e.emit(ArmV6M::mov(ArmV6M::AnyReg(ACC_REG), ArmV6M::AnyReg(window.topReg()))); // materialize now — a bare POP can't safely stay PENDING
                accState.setClean(ACC_REG);
                window.finishPop(e); // must run after the read above — same register
                pc++;
                continue;

            case Op::LOAD: {
                if(!inWindow(window.tos, instr.target)) {
                    e.emit(ArmV6M::ldrSp(R(ACC_REG), ArmV6M::Uoff<2, 8>((uint16_t)window.spillOffset(instr.target))));
                    accState.setClean(ACC_REG);
                    pc++;
                    continue;
                }
                int32_t foldTarget = peekStoreFold(body, bodyCount, pc, window.tos);
                accState.producer(Shape::ofReg(physReg(instr.target)));
                if(foldTarget >= 0) { accState.flush(e, (uint32_t)foldTarget); pc += 2; continue; }
                pc++;
                continue;
            }

            case Op::STORE:
                if(!inWindow(window.tos, instr.target)) {
                    accState.flush(e, ACC_REG);
                    e.emit(ArmV6M::strSp(R(ACC_REG), ArmV6M::Uoff<2, 8>((uint16_t)window.spillOffset(instr.target))));
                    pc++;
                    continue;
                }
                accState.flush(e, physReg(instr.target));
                pc++;
                continue;

            case Op::CONST: {
                int32_t foldTarget = peekStoreFold(body, bodyCount, pc, window.tos);
                uint32_t target = foldTarget >= 0 ? (uint32_t)foldTarget : ACC_REG;
                if(fitsImm8(instr.imm) && foldTarget < 0) {
                    accState.producer(Shape::ofImm(instr.imm)); // stay pending — a later consumer may fold it
                    pc++;
                    continue;
                }
                emitSynthesizeImm32(e, target, (uint32_t)instr.imm);
                accState.setClean(target);
                pc += foldTarget >= 0 ? 2 : 1;
                continue;
            }

            // Every remaining op (arithmetic) carries a combo — the
            // addressing-mode dimension layers onto every one of them
            // uniformly, so they share one dispatch path here.
            default: {
                Combo combo = instr.combo;
                Shape operandStorage{};
                bool hasOperand = true;
                bool popAfter = false;

                if(combo == Combo::REG_ACC || combo == Combo::REG_REG) {
                    if(inWindow(window.tos, instr.target)) operandStorage = Shape::ofReg(physReg(instr.target));
                    else {
                        e.emit(ArmV6M::ldrSp(R(SCRATCH_REG), ArmV6M::Uoff<2, 8>((uint16_t)window.spillOffset(instr.target))));
                        operandStorage = Shape::ofReg(SCRATCH_REG);
                    }
                } else if(combo == Combo::IMM_ACC) {
                    operandStorage = Shape::ofImm(instr.imm);
                } else if(combo == Combo::POP_ACC) {
                    operandStorage = Shape::ofReg(window.topReg());
                    popAfter = true;
                } else {
                    hasOperand = false; // PEEK_PEEK
                }

                bool clobbersAcc = (combo == Combo::REG_REG || combo == Combo::PEEK_PEEK);
                uint32_t dest;
                bool consumedStore = false;
                int32_t storeBackOffset = -1;

                if(combo == Combo::REG_REG) {
                    if(inWindow(window.tos, instr.target)) dest = physReg(instr.target);
                    else { dest = SCRATCH_REG; storeBackOffset = (int32_t)window.spillOffset(instr.target); }
                } else if(combo == Combo::PEEK_PEEK) {
                    dest = window.topReg();
                } else {
                    int32_t foldTarget = peekStoreFold(body, bodyCount, pc, window.tos);
                    dest = foldTarget >= 0 ? (uint32_t)foldTarget : ACC_REG;
                    consumedStore = foldTarget >= 0;
                }

                emitBinary(e, accState, instr.op, combo, hasOperand ? &operandStorage : nullptr, dest, clobbersAcc);
                if(storeBackOffset >= 0) e.emit(ArmV6M::strSp(R(dest), ArmV6M::Uoff<2, 8>((uint16_t)storeBackOffset)));
                if(popAfter) window.finishPop(e);
                pc += consumedStore ? 2 : 1;
                continue;
            }
        }
    }

    return TranslateResult{e.halfwordCount(), e.overflowed()};
}

} // namespace jitc
