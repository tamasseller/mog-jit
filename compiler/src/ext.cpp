// jit-armv6m/compiler — the core-provided half of the extension seam: how
// an extension reaches its own native helpers from emitted code.
//
// Both forms below cost one pooled literal word for the helper's address
// rather than a permanent slot in the flash-resident r10 vector. That
// vector is a fixed core array, so handing out indices in it would make
// every extension's helper set part of the core's own ABI; a pooled word
// needs no index, is deduped within a chunk, and is compaction-safe for
// the same reason every other literal is (the load is pc-relative and the
// pool travels with the code). The cost is pool-reach pressure — poolDebt()
// is charged against SAFE_COND_BRANCH_SPAN — which is why an extension
// declares its pool use up front.
#include "ext.h"
#include "assembler.h"
#include "registers.h"
#include "armv6.h"

#include <cassert>

namespace jitc
{

namespace
{
/* Both forms end the same way: address in r3, then BLX. r3 is the only
 * register Thumb-1 leaves available for this — LDR cannot use a hi
 * register as base, so the vector reach needs a low register, and r0-r2
 * are the staged operands (ext.h). */
void blxVia(Assembler &a, uint32_t reg)
{
    a.emit(ArmV6M::blx(ArmV6M::AnyReg((uint16_t)reg)));
}
} // namespace

void extEmitHelperCall(Assembler &a, const ExtSite &site, uint32_t helperAddr)
{
    // A BLX clobbers lr, which carries the live call/return record — so the
    // prologue must have saved it, and that decision was made from this
    // opcode's declaration back in the pre-pass, long before now.
    assert(extDeclHas(site.decl, EXT_FLAG_NEEDS_LR)); // GCOV_EXCL_LINE
    (void)site;

    a.materializeImm32(ENTRY_JUMP_REG, helperAddr);
    blxVia(a, ENTRY_JUMP_REG);
}

void extEmitCHelperCall(Assembler &a, const ExtSite &site, uint32_t helperAddr)
{
    assert(extDeclHas(site.decl, EXT_FLAG_NEEDS_LR)); // GCOV_EXCL_LINE
    (void)site;

    // Target in r12/ip, which is what runtime.S's extThunkHelper branches
    // to — the AAPCS scratch register, so r0-r3 stay the callee's own
    // arguments. Materialized through r3 first: materializeImm32 targets a
    // low register (Thumb-1 has no hi-register immediate form).
    a.materializeImm32(ENTRY_JUMP_REG, helperAddr);
    a.emit(ArmV6M::mov(ArmV6M::AnyReg(12), ArmV6M::AnyReg(ENTRY_JUMP_REG)));

    // Then the thunk itself, through the r10 vector — the same MOV/LDR/BLX
    // idiom every other helper reach uses (unaryops.cpp, abi_strategy.cpp).
    a.emit(ArmV6M::mov(ArmV6M::AnyReg(ENTRY_JUMP_REG), ArmV6M::AnyReg(HELPER_VEC_REG)));
    a.emit(ArmV6M::ldr(ArmV6M::LoReg(ENTRY_JUMP_REG), ArmV6M::LoReg(ENTRY_JUMP_REG),
        ArmV6M::Uoff<2, 5>((uint16_t)HELPER_EXT_THUNK_OFFSET)));
    blxVia(a, ENTRY_JUMP_REG);
}

void extEmitStateBase(Assembler &a, uint32_t dstLowReg)
{
    // A whole-register MOV out of a hi register, which is one of the three
    // things Thumb-1 lets a hi register do at all (docs/design.md §3) — no
    // mirror tax, unlike anything that would use r9 as a load base.
    a.emit(ArmV6M::mov(ArmV6M::AnyReg((uint16_t)dstLowReg), ArmV6M::AnyReg(RUNTIME_PTR_REG)));
}

} // namespace jitc
