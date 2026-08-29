#include "translate_proc.h"
#include "ext.h"
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
#include "unaryops.h"

#include "runtime_internal.h"

#include <cassert>

namespace jitc
{

using R = ArmV6M::LoReg;
using Cond = ArmV6M::Condition;

static constexpr uint32_t TRANSLATE_BODY_STACK_MARGIN = 224;
 
static bool checkStackFloor(Assembler &a, const Runtime &r)
{
    register uint32_t sp asm("sp");
    if(sp < TRANSLATE_BODY_STACK_MARGIN || sp - TRANSLATE_BODY_STACK_MARGIN < r.liveStackFloor())
    {
        a.fail(RESOURCE_EXHAUSTED_TRANSLATOR_STACK);
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

static FoldResult peekStoreFold(const uint8_t *bytes, uint32_t bytesLen, uint32_t afterPc, uint32_t tos, const ExtHooks *ext)
{
    if(afterPc >= bytesLen)
    {
        return {-1, 0};
    }
    DecodedInstr d = decodeInstr(bytes, bytesLen, afterPc, ext);
    if(d.instr.op == Op::STORE && inWindow(tos, d.instr.target))
    {
        return {(int32_t)physReg(d.instr.target), d.next};
    }
    return {-1, 0};
}

static ArmV6M::Uoff<2, 8> spillImm(Assembler &a, uint32_t byteOffset)
{
    if(!ArmV6M::Uoff<2, 8>::isInRange(byteOffset))
    {
        a.fail(RESOURCE_LIMIT_SPILL_OFFSET);
    }
    return ArmV6M::Uoff<2, 8>((uint16_t)byteOffset);
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

enum class BranchWidth
{
    Narrow, Wide
};

bool emitGuardedBranch(Assembler &a, Label &label, Cond condition, BranchWidth width)
{
    switch(width)
    {
        case BranchWidth::Narrow:
            return a.branchTo(label, condition);

        case BranchWidth::Wide:
        {
            Label fallThrough;

            const auto branchOk = a.branchTo(fallThrough, ArmV6M::inverse(condition));
            assert(branchOk);

            if(!a.branchTo(label))
            {
                return false;
            }

            const auto bindOk = a.bind(fallThrough);
            assert(bindOk);

            return true;
        }
        default:
            assert(false);
            return false;
    }
}

struct Ctx
{
    Assembler a;
    Window window;
    const uint8_t *bytes;
    uint32_t bytesLen;
    uint32_t procIdx;
    bool savesLR;
    uint32_t initialSpilledCount;

    bool hasPendingComparisonCondition = false;
    Cond pendingComparisonCondition = Cond::EQ;

    AccState accState;

    Ctx(Runtime& r, uint32_t procIdx, uint32_t lruTick): a(&r, procIdx, lruTick)
    {
        const ProcSlot &procSlot = r.slot(procIdx);

        this->window = Window{procSlot.argCount(), procSlot.needsLRSave()};
        this->bytes = (const uint8_t *)(uintptr_t)procSlot.bodyPtr;
        this->bytesLen = procSlot.bodyBytes();
        this->savesLR = procSlot.needsLRSave();
        this->procIdx = procIdx; 
        this->initialSpilledCount = procSlot.argCount() > WINDOW_SIZE ? procSlot.argCount() - WINDOW_SIZE : 0;
    }

    void returnSequence();
    void localJumpCleanup(uint32_t tos);
    void globalJumpCleanup(uint32_t tos);
    void handleGlobalJump(Instr term, uint32_t tos);

    ArmV6M::Condition handleComparisonEmission(const Instr &instr);

    bool processUntilTerminator(uint32_t pc, BranchWidth width, bool isThisLoopCondBlock, DecodedInstr &out);
    uint32_t translateLoop(uint32_t pc, BranchWidth width);
    uint32_t translateIfThen(uint32_t pc, BranchWidth width);
    uint32_t translateIfThenElse(uint32_t pc, BranchWidth width);
    uint32_t translateSwitch(uint32_t pc, BranchWidth width, uint32_t n);
    bool translateBody(BranchWidth width);
};

void Ctx::returnSequence()
{
    this->window.discard(a);
    abiEmitReturn(a, this->savesLR, this->initialSpilledCount);
}

ArmV6M::Condition Ctx::handleComparisonEmission(const Instr &instr)
{
    Combo combo = instr.combo;

    if(combo == Combo::REG_ACC || combo == Combo::REG_REG)
    {
        if(inWindow(this->window.tos, instr.target))
        {
            return emitComparison(a, this->accState, instr.op, Shape::ofReg(physReg(instr.target)));
        }
        else
        {
            a.emit(ArmV6M::ldrSp(R(SCRATCH_REG), spillImm(a, this->window.spillOffset(instr.target))));
            return emitComparison(a, this->accState, instr.op, Shape::ofReg(SCRATCH_REG));
        }
    }
    else if(combo == Combo::IMM_ACC)
    {
        return emitComparison(a, this->accState, instr.op, Shape::ofImm(instr.imm));
    }
    else 
    {
        const auto ret = emitComparison(a, this->accState, instr.op, Shape::ofReg(this->window.topReg()));
        
        if(combo == Combo::POP_ACC)
        {
            this->window.finishPop(a);
        }

        return ret;
    }
}

bool Ctx::processUntilTerminator(uint32_t pc, BranchWidth width, bool isThisLoopCondBlock, DecodedInstr& out)
{
    while(pc < this->bytesLen)
    {
        DecodedInstr decoded = decodeInstr(this->bytes, this->bytesLen, pc, this->a.r().extension());
        const Instr &instr = decoded.instr;
        uint32_t afterInstr = decoded.next;

        switch(instr.op)
        {
            case Op::EXT:
            {
                const ExtHooks *hooks = this->a.r().extension();
                if(hooks == nullptr || hooks->emit == nullptr)
                {
                    a.fail(RESOURCE_PROGRAM_EXT_UNKNOWN);
                    pc = afterInstr; // per fail()'s own contract
                    break;
                }

                const uint32_t decl = instr.extDecl;
                const uint32_t pops = (uint32_t)(-extDeclTosDelta(decl));
                const bool readsAcc = extDeclHas(decl, EXT_FLAG_READS_ACC);
                const bool writesAcc = extDeclHas(decl, EXT_FLAG_WRITES_ACC);

                ExtSite site{};
                site.bytes = this->bytes;
                site.bytesLen = this->bytesLen;
                site.pc = pc;
                site.decl = decl;
                site.out = (uint8_t)ACC_REG;
                site.scratch = (1u << ENTRY_JUMP_REG) | (1u << 12);

                if(readsAcc)
                {
                    this->accState.flush(a, ACC_REG);
                    site.in[site.inCount++] = (uint8_t)ACC_REG;
                }
                else if(pops > 0 || writesAcc)
                {
                    // Staging clobbers r1/r2, and a deferred acc must not be
                    // left depending on either. flushLive, not flush: a
                    // poisoned acc is legitimate here and stays poisoned.
                    this->accState.flushLive(a, ACC_REG);
                }

                // Top first, into r1 then r2 — never r0, which is acc's, and
                // never r3, which is the only register a helper reach can use.
                static const uint8_t STACK_STAGE[EXT_MAX_STACK_INPUTS] = {ENTRY_IDX_REG, SCRATCH_REG};
                for(uint32_t i = 0; i < pops; i++)
                {
                    uint32_t src = this->window.topReg();
                    uint8_t dst = STACK_STAGE[i];
                    if(src != dst)
                    {
                        a.emit(ArmV6M::mov(ArmV6M::AnyReg(dst), ArmV6M::AnyReg(src)));
                    }
                    site.in[site.inCount++] = dst;
                    this->window.finishPop(a);
                }

                const uint32_t before = a.pc();
                hooks->emit(a, site);

                if(a.pc() - before > extDeclHalfwords(decl) * 2)
                {
                    a.fail(RESOURCE_PROGRAM_EXT_UNSUPPORTED);
                    pc = afterInstr;
                    break;
                }

                if(writesAcc)
                {
                    this->accState.setClean(ACC_REG);
                }

                pc = afterInstr;
                break;
            }
            case Op::CALL:
            {
                if(instr.calleeIndex >= this->a.r().procCount)
                {
                    a.fail(RESOURCE_PROGRAM_CALLEE_RANGE);
                    pc = afterInstr;
                    break;
                }

                uint32_t calleeArgCount = this->a.r().slot(instr.calleeIndex).argCount();
                uint32_t stackArgs = calleeArgCount > 0 ? calleeArgCount - 1 : 0;

                if(calleeArgCount > 0)
                {
                    this->accState.flush(a, ACC_REG);
                }
                else
                {
                    this->accState.poison();
                }

                this->window.spillForCall(a, stackArgs);
                Window::fillCalleeArgs(a, stackArgs);
                abiEmitCall(a, this->procIdx, instr.calleeIndex);
                this->window.reloadAfterCall(a, this->window.tos - stackArgs);

                this->accState.producer(Shape::ofReg(ACC_REG));
                pc = afterInstr;
                break;
            }

            case Op::PUSH:
                this->window.pushValue(a, this->accState);
                pc = afterInstr;
                break;

            case Op::POP:
                a.emit(ArmV6M::mov(ArmV6M::AnyReg(ACC_REG), ArmV6M::AnyReg(this->window.topReg()))); // materialize now — a bare POP can't safely stay PENDING
                this->accState.setClean(ACC_REG);
                this->window.finishPop(a); // must run after the read above — same register

                pc = afterInstr;
                break;

            case Op::NEG:
            case Op::NOT:
            {
                FoldResult fold = peekStoreFold(this->bytes, this->bytesLen, afterInstr, this->window.tos, this->a.r().extension());
                uint32_t dest = fold.redirectReg(ACC_REG);
                uint32_t src = this->accState.peek().peek(a, dest);

                emitUnary(a, instr.op, dest, src);
                this->accState.setClean(dest);
                
                pc = fold.redirectAfterNext(afterInstr);
                break;
            }
            case Op::CLZ:
            case Op::REVBITS:
            {
                this->accState.flush(a, ACC_REG);
                FoldResult fold = peekStoreFold(this->bytes, this->bytesLen, afterInstr, this->window.tos, this->a.r().extension());
                uint32_t dest = fold.redirectReg(ACC_REG);

                emitUnary(a, instr.op, dest, ACC_REG);
                this->accState.setClean(dest);
                
                pc = fold.redirectAfterNext(afterInstr);
                break;
            }
            case Op::LOAD:
            {
                if(!inWindow(this->window.tos, instr.target))
                {
                    a.emit(ArmV6M::ldrSp(R(ACC_REG), spillImm(a, this->window.spillOffset(instr.target))));
                    this->accState.setClean(ACC_REG);

                    pc = afterInstr;
                    break;
                }

                FoldResult fold = peekStoreFold(this->bytes, this->bytesLen, afterInstr, this->window.tos, this->a.r().extension());
                
                this->accState.producer(Shape::ofReg(physReg(instr.target)));

                if(fold.reg >= 0)
                {
                    this->accState.flush(a, (uint32_t)fold.reg);

                    pc = fold.afterNext;
                    break;
                }

                pc = afterInstr;
                break;
            }

            case Op::STORE:
                if(!inWindow(this->window.tos, instr.target))
                {
                    uint32_t r = this->accState.peek().peek(a, SCRATCH_REG);
                    a.emit(ArmV6M::strSp(R(r), spillImm(a, this->window.spillOffset(instr.target))));

                    pc = afterInstr;
                    break;
                }
                else
                {
                    this->accState.flush(a, physReg(instr.target));
                    
                    pc = afterInstr;
                    break;
                }

            case Op::CONST:
            {
                if(fitsImm8(instr.imm))
                {
                    this->accState.producer(Shape::ofImm(instr.imm)); // stay pending — a later consumer may fold it

                    pc = afterInstr;
                    break;
                }
                else
                {
                    FoldResult fold = peekStoreFold(this->bytes, this->bytesLen, afterInstr, this->window.tos, this->a.r().extension());
                    uint32_t target = fold.redirectReg(ACC_REG);
                    a.materializeImm32(target, (uint32_t)instr.imm);
                    this->accState.setClean(target);

                    pc = fold.redirectAfterNext(afterInstr);
                    break;
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
                        if(inWindow(this->window.tos, instr.target))
                        {
                            operandStorage = Shape::ofReg(physReg(instr.target));
                        }
                        else
                        {
                            a.emit(ArmV6M::ldrSp(R(SCRATCH_REG), spillImm(a, this->window.spillOffset(instr.target))));
                            operandStorage = Shape::ofReg(SCRATCH_REG);
                        }
                        break;
                    case Combo::IMM_ACC:
                        operandStorage = Shape::ofImm(instr.imm);
                        break;
                    case Combo::POP_ACC:
                    case Combo::PEEK_PEEK:
                        operandStorage = Shape::ofReg(this->window.topReg());
                        break;
                    default: assert(false);
                };

                if(instr.combo == Combo::REG_REG)
                {
                    if(inWindow(this->window.tos, instr.target))
                    {
                        emitBinaryOp(a, instr.op, instr.combo, this->accState.peek(), operandStorage, physReg(instr.target));
                    }
                    else
                    {
                        emitBinaryOp(a, instr.op, instr.combo, this->accState.peek(), operandStorage, SCRATCH_REG);
                        a.emit(ArmV6M::strSp(R(SCRATCH_REG), spillImm(a, this->window.spillOffset(instr.target))));
                    }

                    this->accState.poison();
                    pc = afterInstr;
                    break;
                }
                else if(instr.combo == Combo::PEEK_PEEK)
                {
                    emitBinaryOp(a, instr.op, instr.combo, this->accState.peek(), operandStorage, this->window.topReg());
                    this->accState.poison();
                    pc = afterInstr;
                    break;
                }
                else
                {
                    FoldResult fold = peekStoreFold(this->bytes, this->bytesLen, afterInstr, this->window.tos, this->a.r().extension());

                    const auto dest = fold.redirectReg(ACC_REG);
                    emitBinaryOp(a, instr.op, instr.combo, this->accState.peek(), operandStorage, dest);
                    this->accState.setClean(dest);

                    if(instr.combo == Combo::POP_ACC)
                    {
                        this->window.finishPop(a);
                    }

                    pc = fold.redirectAfterNext(afterInstr);
                    break;
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
                Cond trueCondition = this->handleComparisonEmission(instr);

                if(afterInstr < this->bytesLen)
                {
                    DecodedInstr lookahead = decodeInstr(this->bytes, this->bytesLen, afterInstr, this->a.r().extension());
                    
                    bool fusesIntoBrTable = lookahead.instr.op == Op::BR_TABLE && (lookahead.instr.imm == 1 || lookahead.instr.imm == 2);
                    bool fusesIntoLoopExit = isThisLoopCondBlock && lookahead.instr.op == Op::BLOCK_END;

                    if(fusesIntoBrTable || fusesIntoLoopExit)
                    {
                        this->pendingComparisonCondition = trueCondition;
                        this->hasPendingComparisonCondition = true;
                        this->accState.poison();
                        
                        pc = afterInstr;
                        break;
                    }
                }

                FoldResult fold = peekStoreFold(this->bytes, this->bytesLen, afterInstr, this->window.tos, this->a.r().extension());
                uint32_t dest = fold.redirectReg(ACC_REG);

                Label falseLabel;
                if(!a.branchTo(falseLabel, ArmV6M::inverse(trueCondition))) return false;
                
                a.emit(ArmV6M::movs(R((uint16_t)dest), ArmV6M::Imm<8>(1)));

                Label endLabel;
                const auto endOk = a.branchTo(endLabel);
                assert(endOk);

                const auto falseOk = a.bind(falseLabel);
                assert(falseOk);

                a.emit(ArmV6M::movs(R((uint16_t)dest), ArmV6M::Imm<8>(0)));
                
                const auto endBound = a.bind(endLabel);
                assert(endBound);

                this->accState.setClean(dest);

                pc = fold.redirectAfterNext(afterInstr);
                break;
            }
            
            case Op::LOOP: 
                pc = this->translateLoop(afterInstr, width); 

                if(pc == -1) 
                {
                    return false;
                }

                break;

            case Op::BR_TABLE:
                switch(instr.imm)
                {
                    case 1: pc = this->translateIfThen(afterInstr, width); break;
                    case 2: pc = this->translateIfThenElse(afterInstr, width); break;
                    default: pc = this->translateSwitch(afterInstr, width, instr.imm);break;
                }

                if(pc == -1) 
                {
                    return false;
                }

                break;

            default:
                assert(isTerminator(instr));
                assert(!this->hasPendingComparisonCondition || (isThisLoopCondBlock && instr.op == Op::BLOCK_END)); // GCOV_EXCL_LINE — comparison fused into nothing; malformed program

                out = DecodedInstr{.instr = instr, .next = afterInstr};
                return true;
        }
    }

    assert(false); // GCOV_EXCL_LINE
    return false;
}

void Ctx::localJumpCleanup(uint32_t tos)
{
    this->accState.flushLive(a, ACC_REG);
    this->window.restore(a, tos);
}

void Ctx::globalJumpCleanup(uint32_t tos)
{
    this->accState.poison();
    this->window.tos = tos;
}

void Ctx::handleGlobalJump(Instr term, uint32_t tos)
{
    if(term.op == Op::RETURN)
    {
        this->accState.flush(a, ACC_REG);
        this->returnSequence();
    }
    else
    {
        assert(term.op == Op::TRAP);

        a.materializeImm32(ACC_REG, (uint32_t)term.imm);
        abiEmitTrap(a);
    }

    this->globalJumpCleanup(tos);
}

uint32_t Ctx::translateIfThen(uint32_t pc, BranchWidth width)
{
    if(!checkStackFloor(a, this->a.r()))
    {
        return -1;
    }

    const auto entryTos = this->window.tos;
    const bool fused = this->hasPendingComparisonCondition;
    this->hasPendingComparisonCondition = false;

    Label skip;

    const auto cond = fused ? this->pendingComparisonCondition : testAccNonzero(a, this->accState);

    if(!emitGuardedBranch(a, skip, cond, width))
    {
        return -1;
    }

    if(fused)
    {
        this->accState.producer(Shape::ofImm(0));
    }
    
    if(DecodedInstr term; this->processUntilTerminator(pc, width, false, term))
    {
        if(term.instr.op == Op::BLOCK_END)
        {
            this->localJumpCleanup(entryTos);
        }
        else
        {
            this->handleGlobalJump(term.instr, entryTos);
        }

        this->accState.poison();

        if(a.bind(skip))
        {
            return term.next;
        }
    }

    return -1;
}

uint32_t Ctx::translateIfThenElse(uint32_t pc, BranchWidth width)
{
    if(!checkStackFloor(this->a, this->a.r()))
    {
        return -1;
    }

    const auto entryTos = this->window.tos;
    const bool fused = this->hasPendingComparisonCondition;
    this->hasPendingComparisonCondition = false;

    Label end, otherwise;

    if(fused)
    {
        if(!emitGuardedBranch(a, otherwise, this->pendingComparisonCondition, width))
        {
            return -1;
        }
    }
    else
    {
        this->accState.flush(a, ACC_REG);

        a.emit(ArmV6M::cmp(R(ACC_REG), ArmV6M::Imm<8>(1)));

        if(!emitGuardedBranch(a, end, Cond::HI, width))
        {
            return -1;
        }

        a.emit(ArmV6M::cmp(R(ACC_REG), ArmV6M::Imm<8>(1)));

        if(!emitGuardedBranch(a, otherwise, Cond::EQ, width))
        {
            return -1;
        }
    }

    this->accState.producer(Shape::ofImm(0));

    if(DecodedInstr term; this->processUntilTerminator(pc, width, false, term))
    {
        if(term.instr.op == Op::BLOCK_END)
        {
            this->localJumpCleanup(entryTos);
            if(!a.branchTo(end))
            {
                return -1;
            }
        }
        else
        {
            this->handleGlobalJump(term.instr, entryTos);
        }

        a.flushPoolNoGuard();
        if(!a.bind(otherwise))
        {
            return -1;
        }

        this->accState.producer(Shape::ofImm(1));

        if(DecodedInstr term2; this->processUntilTerminator(term.next, width, false, term2))
        {
            if(term2.instr.op == Op::BLOCK_END)
            {
                this->localJumpCleanup(entryTos);
            }
            else
            {
                this->handleGlobalJump(term2.instr, entryTos);
            }

            this->accState.poison();

            if(end.chain != -1)
            {
                if(!a.bind(end))
                {
                    return -1;
                }
            }

            return term2.next;
        }
    }

    return -1;
}

uint32_t Ctx::translateSwitch(uint32_t pc, BranchWidth width, uint32_t n)
{
    if(!checkStackFloor(a, this->a.r()))
    {
        return -1;
    }

    const auto entryTos = this->window.tos;
    assert(this->hasPendingComparisonCondition == false);

    this->accState.flush(a, ACC_REG);
    a.materializeImm32(SCRATCH_REG, n);

    uint32_t tableBytes = 6 + (n + 1) * 2;

    uint32_t base;
    {
        Assembler::AtomicBlock atomic(a, /*poolEntries=*/0, tableBytes);
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

    for(uint32_t i = 0; i < n; i++)
    {
        a.patchRawHalfword(base + i * 2, (uint16_t)(a.pc() - base));

        this->accState.poison();

        if(DecodedInstr term; this->processUntilTerminator(pc, width, false, term))
        {
            if(term.instr.op == Op::BLOCK_END)
            {
                this->localJumpCleanup(entryTos);
                if(i + 1 < n)
                {
                    if(!a.branchTo(end))
                    {
                        return -1;
                    }

                    a.flushPoolNoGuard();
                }
            }
            else
            {
                this->handleGlobalJump(term.instr, entryTos);
                a.flushPoolNoGuard();
            }

            pc = term.next;
        }
        else
        {
            return -1;
        }
    }

    a.patchRawHalfword(base + n * 2, (uint16_t)(a.pc() - base));

    this->accState.poison();

    if(end.chain != -1)
    {
        if(!a.bind(end))
        {
            return -1;
        }
    }

    return pc;
}

uint32_t Ctx::translateLoop(uint32_t pc, BranchWidth width)
{
    if(!checkStackFloor(a, this->a.r()))
    {
        return -1;
    }

    const auto entryTos = this->window.tos;

    this->accState.flushLive(a, ACC_REG);
    const auto start = a.pc();

    if(DecodedInstr condTerm; this->processUntilTerminator(pc, width, true, condTerm))
    {
        assert(condTerm.instr.op == Op::BLOCK_END);

        if(this->window.tos != entryTos)
        {
            this->accState.flushLive(a, ACC_REG);
            this->window.restore(a, entryTos);
        }

        const bool fused = this->hasPendingComparisonCondition;
        this->hasPendingComparisonCondition = false;

        const auto cond = fused ? this->pendingComparisonCondition : testAccNonzero(a, this->accState);

        Label out;
        if(!emitGuardedBranch(a, out, ArmV6M::inverse(cond), width))
        {
            return -1;
        }

        if(fused)
        {
            this->accState.producer(Shape::ofImm(1));
        }

        if(DecodedInstr bodyTerm; this->processUntilTerminator(condTerm.next, width, false, bodyTerm))
        {
            if(bodyTerm.instr.op == Op::BLOCK_END)
            {
                this->localJumpCleanup(entryTos);
                int32_t delta = (int32_t)start - (int32_t)(a.pc() + 4);
                if(!ArmV6M::Ioff<1, 11>::isInRange(delta))
                {
                    a.fail(RESOURCE_LIMIT_LOOP_BACK_EDGE);
                    return -1;
                }
                a.emit(ArmV6M::b(ArmV6M::Ioff<1, 11>((int16_t)delta)));
            }
            else
            {
                this->handleGlobalJump(bodyTerm.instr, entryTos);
            }

            a.flushPoolNoGuard();

            this->accState.poison();

            if(!a.bind(out))
            {
                return -1;
            }

            return bodyTerm.next;
        }
    }
    
    return -1;
}

bool Ctx::translateBody(BranchWidth width)
{
    if(checkStackFloor(a, this->a.r()))
    {
        if(DecodedInstr decoded; processUntilTerminator(0, width, false, decoded))
        {
            const Instr &instr = decoded.instr;

            assert(instr.op == Op::RETURN || instr.op == Op::TRAP); // GCOV_EXCL_LINE — BLOCK_END with no open block; malformed program
            this->handleGlobalJump(instr, this->window.tos);
            assert(decoded.next == this->bytesLen);

            return true;
        }
    }

    return false;
}

uint32_t translateProc(uint32_t procIdx, Runtime& r, uint32_t lruTick)
{
    Ctx ctx(r, procIdx, lruTick);

    abiEmitPrologue(ctx.a, ctx.savesLR);

    if(ctx.window.tos >= 1)
    {
        ctx.accState.flush(ctx.a, physReg(ctx.window.tos - 1));
    }

    if(!ctx.translateBody(BranchWidth::Narrow))
    {
        if(ctx.translateBody(BranchWidth::Wide))
        {
            ctx.a.fail(RESOURCE_LIMIT_BRANCH_RANGE);
        }
    }

    return ctx.a.finalize();
}

} // namespace jitc
