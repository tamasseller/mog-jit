#include "unaryops.h"
#include "emitter.h"
#include "registers.h"
#include "armv6.h"

#include <cassert>

namespace jitc {

using R = ArmV6M::LoReg;

namespace {
constexpr uint32_t LR = 14;
}

void emitUnary(Emitter &e, Op op, uint32_t dest, UnaryHelperSites &helperSites) {
    if(op == Op::NEG) { e.emit(ArmV6M::negs(R((uint16_t)dest), R(ACC_REG))); return; }
    if(op == Op::NOT) { e.emit(ArmV6M::mvns(R((uint16_t)dest), R(ACC_REG))); return; }

    uint32_t site = e.placeholderBL();
    if(op == Op::CLZ) {
        assert(helperSites.clzCount < kMaxUnaryHelperSites); // GCOV_EXCL_LINE — a procedure with more CLZ sites than this needs kMaxUnaryHelperSites raised
        helperSites.clz[helperSites.clzCount++] = site;
    } else {
        assert(op == Op::REVBITS); // GCOV_EXCL_LINE — emitUnary's own caller already checked the op
        assert(helperSites.revbitsCount < kMaxUnaryHelperSites); // GCOV_EXCL_LINE — see the CLZ branch's own comment
        helperSites.revbits[helperSites.revbitsCount++] = site;
    }
    if(dest != ACC_REG) e.emit(ArmV6M::mov(ArmV6M::AnyReg((uint16_t)dest), ArmV6M::AnyReg((uint16_t)ACC_REG)));
}

uint32_t emitClzHelper(Emitter &e) {
    constexpr uint32_t COUNT_REG = 1;
    uint32_t start = e.pc();
    e.emit(ArmV6M::movs(R(COUNT_REG), ArmV6M::Imm<8>(0)));
    e.emit(ArmV6M::cmp(R(ACC_REG), ArmV6M::Imm<8>(0)));
    uint32_t zeroSite = e.placeholderCondBranch(ArmV6M::Condition::EQ);
    uint32_t loopStart = e.pc();
    e.emit(ArmV6M::lsls(R(ACC_REG), R(ACC_REG), ArmV6M::Imm<5>(1))); // carry = the bit shifted out, i.e. the old bit 31
    uint32_t doneSite = e.placeholderCondBranch(ArmV6M::Condition::HS); // carry set == that bit was 1
    e.emit(ArmV6M::adds(R(COUNT_REG), ArmV6M::Imm<8>(1)));
    e.emit(ArmV6M::b(ArmV6M::Ioff<1, 11>((int16_t)((int32_t)loopStart - (int32_t)(e.pc() + 4)))));
    e.patchBranch(doneSite, e.pc());
    e.emit(ArmV6M::mov(ArmV6M::AnyReg(ACC_REG), ArmV6M::AnyReg(COUNT_REG)));
    e.emit(ArmV6M::bx(ArmV6M::AnyReg(LR)));
    e.patchBranch(zeroSite, e.pc());
    e.emit(ArmV6M::movs(R(ACC_REG), ArmV6M::Imm<8>(32)));
    e.emit(ArmV6M::bx(ArmV6M::AnyReg(LR)));
    return start;
}

uint32_t emitRevbitsHelper(Emitter &e) {
    constexpr uint32_t SRC_REG = 1;
    uint32_t start = e.pc();
    e.emit(ArmV6M::mov(ArmV6M::AnyReg(SRC_REG), ArmV6M::AnyReg(ACC_REG)));
    e.emit(ArmV6M::movs(R(ACC_REG), ArmV6M::Imm<8>(0)));
    e.emit(ArmV6M::movs(R(SCRATCH_REG), ArmV6M::Imm<8>(32)));
    uint32_t loopStart = e.pc();
    e.emit(ArmV6M::lsrs(R(SRC_REG), R(SRC_REG), ArmV6M::Imm<5>(1))); // carry = the bit shifted out, i.e. the old bit 0
    e.emit(ArmV6M::adcs(R(ACC_REG), R(ACC_REG)));
    e.emit(ArmV6M::subs(R(SCRATCH_REG), ArmV6M::Imm<8>(1)));
    uint32_t doneSite = e.placeholderCondBranch(ArmV6M::Condition::EQ);
    e.emit(ArmV6M::b(ArmV6M::Ioff<1, 11>((int16_t)((int32_t)loopStart - (int32_t)(e.pc() + 4)))));
    e.patchBranch(doneSite, e.pc());
    e.emit(ArmV6M::bx(ArmV6M::AnyReg(LR)));
    return start;
}

} // namespace jitc
