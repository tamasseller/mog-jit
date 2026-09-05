/* The raw-memory test extension, target half — the mirror of
 * fuzz/ts/lib/rawmem_ext.ts, which carries the specification.
 *
 * The buffer is static and its address is therefore a link-time constant,
 * so nothing here needs per-excursion extension state (design.md §18.1):
 * every site materializes the base as a pooled literal.
 *
 * Offsets are masked to the buffer size and aligned down to the access
 * width by shifts alone, so no access can leave the buffer and none can be
 * unaligned — that is the whole safety argument, and it is why the emitted
 * code carries no bounds check.
 *
 * Loads and stores are shaped the way the core's own ops are: a load is a
 * unary transform on acc, a store takes its value from acc like STORE
 * does. MEMMOVE is the deliberate exception — three operands, all off the
 * stack, none of them acc — and it runs through a hand-written Thumb helper
 * (ext_rawmem_helper.S) reached by ExtSite::helperCall, which is what puts
 * r0 in play as an argument register and makes accInvalidate load-bearing
 * here. It takes a destination range rather than a length; see emitMemmove. */

#include "ext_rawmem.h"

#include "ext.h"
#include "assembler.h"
#include "registers.h"
#include "armv6.h"

using namespace jitc;

using R = ArmV6M::LoReg;

uint8_t g_rawMem[RAWMEM_BYTES] __attribute__((aligned(4)));

/* ext_rawmem_helper.S — .thumb_func, so the address carries its own Thumb
 * bit exactly as the core's own clzHelper does. */
extern "C" const uint16_t rawMemMoveHelper[];

/* Ordinary AAPCS C, reached through extThunkHelper rather than directly:
 * that is what realigns sp to 8 and preserves lr across the call. Two
 * three arguments, in r0-r2: REALIGN_ENTER spends lr and r3 rather than a
 * second low register, and r3 arrives pointing at the stack the call was
 * made over, for anything that needs more than three.
 *
 * Unlike the hand-rolled MEMMOVE helper this one is free to use r4/r5,
 * because being ordinary C it saves and restores them. Its frame measures
 * 12 bytes (-fstack-usage); with EXT_THUNK_STACK_BYTES that is 24, well
 * under the TRANSLATOR_ENTRY_WORST_CASE_BYTES that Executor::run takes the
 * max against, so the up-front reservation covers it without this
 * extension declaring an extHelperStackBytes of its own. */
extern "C" int32_t rawMemCmpHelper(uint32_t a, uint32_t aEnd, uint32_t b)
{
    while(a < aEnd)
    {
        const uint8_t x = g_rawMem[a];
        const uint8_t y = g_rawMem[b];

        if(x != y)
        {
            return (int32_t)x - (int32_t)y;
        }

        a++;
        b = (b + 1) & RAWMEM_ADDR_MASK;
    }

    return 0;
}

namespace
{
constexpr uint32_t OFF_REG = 1;   // r1
constexpr uint32_t VAL_REG = 2;   // r2
constexpr uint32_t BASE_REG = 3;  // r3

constexpr uint32_t ADDR_BITS = 10; // the whole buffer

uint32_t widthOf(uint8_t opcode)
{
    switch(opcode)
    {
        case RAWMEM_LD8:
        case RAWMEM_ST8: return 1;
        case RAWMEM_LD16:
        case RAWMEM_ST16: return 2;
        default: return 4;
    }
}

uint32_t log2Of(uint32_t width)
{
    return width == 1 ? 0 : (width == 2 ? 1 : 2);
}

/* Mask to ADDR_BITS and align down to `width`, in three shifts and no
 * scratch register — Thumb-1 has no AND-with-immediate. */
void maskAndAlign(Assembler &a, uint32_t reg, uint32_t width)
{
    const uint32_t drop = 32 - ADDR_BITS;
    const uint32_t k = log2Of(width);

    a.emit(ArmV6M::lsls(R((uint16_t)reg), R((uint16_t)reg), ArmV6M::Imm<5>((uint16_t)drop)));
    a.emit(ArmV6M::lsrs(R((uint16_t)reg), R((uint16_t)reg), ArmV6M::Imm<5>((uint16_t)(drop + k))));

    if(k != 0)
    {
        a.emit(ArmV6M::lsls(R((uint16_t)reg), R((uint16_t)reg), ArmV6M::Imm<5>((uint16_t)k)));
    }
}

/* acc = mem[acc], a unary transform on the accumulator like NEG or CLZ:
 * the address arrives in acc and the value replaces it, touching the
 * operand stack not at all. */
void emitLoad(ExtSite &site, uint32_t width)
{
    site.accInto(ACC_REG);
    maskAndAlign(site.a, ACC_REG, width);
    site.a.materializeImm32(BASE_REG, (uint32_t)(uintptr_t)g_rawMem);

    const R dst((uint16_t)ACC_REG), base((uint16_t)BASE_REG), off((uint16_t)ACC_REG);
    site.a.emit(width == 1 ? ArmV6M::ldrb(dst, base, off)
        : (width == 2 ? ArmV6M::ldrh(dst, base, off) : ArmV6M::ldr(dst, base, off)));

    site.accIsNowIn(ACC_REG);
}

/* mem[pop()] = acc, mirroring the core's own STORE: acc carries the value,
 * and the only dynamic part — the address — comes off the stack. */
void emitStore(ExtSite &site, uint32_t width)
{
    site.pop(OFF_REG);
    // The value stays in acc's own register: nothing below touches it, and
    // a store leaves acc readable (no writesAcc in the effect table).
    site.accInto(ACC_REG);
    maskAndAlign(site.a, OFF_REG, width);
    site.a.materializeImm32(BASE_REG, (uint32_t)(uintptr_t)g_rawMem);

    const R val((uint16_t)ACC_REG), base((uint16_t)BASE_REG), off((uint16_t)OFF_REG);
    site.a.emit(width == 1 ? ArmV6M::strb(val, base, off)
        : (width == 2 ? ArmV6M::strh(val, base, off) : ArmV6M::str(val, base, off)));
}

/* src, dstStart, dstEnd off the stack into the helper's own argument
 * registers, then the raw helper reach: a pooled address and a BLX.
 *
 * A destination *range* rather than a length is what lets all three mask to
 * the full buffer: a length would have to be masked conservatively enough
 * that start + len still fit, costing half the address space, and would
 * turn a length of exactly the buffer size into zero.
 *
 * accInvalidate comes first, not last. r0 is an argument register from the
 * first pop onward, and a later pop that uncovers a spill would otherwise
 * resolve a live accumulator *into r0* and overwrite what is staged there.
 * Poisoning up front is what stops that resolution firing. */
void emitMemmove(ExtSite &site)
{
    const uint32_t SRC = ACC_REG, DST_START = OFF_REG, DST_END = VAL_REG;

    site.accInvalidate();

    site.pop(DST_END);
    site.pop(DST_START);
    site.pop(SRC);

    maskAndAlign(site.a, SRC, 1);
    maskAndAlign(site.a, DST_START, 1);
    maskAndAlign(site.a, DST_END, 1);

    site.helperCall((uint32_t)(uintptr_t)rawMemMoveHelper);
}

/* Four operands, which is one more than the argument registers hold: the
 * fourth is pushed, and the thunk hands the helper r3 addressing it.
 *
 * Nothing sp-relative may happen between that push and its matching
 * release — load/store on a spilled slot compute their offset from
 * window.spillOffset, which assumes sp is where window.tos says it is. */
extern "C" int32_t rawMemSliceCmpHelper(uint32_t aStart, uint32_t aEnd, uint32_t bStart,
    const uint32_t *stack)
{
    const uint32_t bEnd = stack[0];

    const uint32_t lenA = aEnd > aStart ? aEnd - aStart : 0;
    const uint32_t lenB = bEnd > bStart ? bEnd - bStart : 0;
    const uint32_t common = lenA < lenB ? lenA : lenB;

    for(uint32_t i = 0; i < common; i++)
    {
        const uint8_t x = g_rawMem[aStart + i];
        const uint8_t y = g_rawMem[bStart + i];

        if(x != y)
        {
            return (int32_t)x - (int32_t)y;
        }
    }

    return (int32_t)lenA - (int32_t)lenB;
}

/* The same operand shape as MEMMOVE, but through the C reach: all three
 * operands go straight into r0-r2 as ordinary AAPCS arguments. */
void emitMemcmp(ExtSite &site)
{
    const uint32_t A_START = ACC_REG, A_END = OFF_REG, B_START = VAL_REG;

    site.accInvalidate(); // r0 carries an argument before it carries the result

    site.pop(B_START);
    site.pop(A_END);
    site.pop(A_START);

    maskAndAlign(site.a, A_START, 1);
    maskAndAlign(site.a, A_END, 1);
    maskAndAlign(site.a, B_START, 1);

    site.cHelperCall((uint32_t)(uintptr_t)rawMemCmpHelper);

    site.accIsNowIn(ACC_REG); // AAPCS return value
}

/* The escape hatch: three operands ride the argument registers and the
 * fourth rides the stack, where extThunkHelper's r3 points. */
void emitSliceCmp(ExtSite &site)
{
    const uint32_t A_START = ACC_REG, A_END = OFF_REG, B_START = VAL_REG, B_END = BASE_REG;

    site.accInvalidate();

    site.pop(B_END);
    site.pop(B_START);
    site.pop(A_END);
    site.pop(A_START);

    maskAndAlign(site.a, A_START, 1);
    maskAndAlign(site.a, A_END, 1);
    maskAndAlign(site.a, B_START, 1);
    maskAndAlign(site.a, B_END, 1);

    ArmV6M::LoRegs fourth{0};
    fourth.add(R((uint16_t)B_END));
    site.a.emit(ArmV6M::push(fourth));

    site.cHelperCall((uint32_t)(uintptr_t)rawMemSliceCmpHelper);

    site.a.emit(ArmV6M::incrSp(ArmV6M::Uoff<2, 7>(4))); // release it again

    site.accIsNowIn(ACC_REG);
}
} // namespace

/* The opcode byte is the whole instruction for every op here, so neither
 * phase reads anything off the wire. */
extern "C" bool extDescribe(uint8_t opcode, BcReader &, uint32_t *desc)
{
    switch(opcode)
    {
        case RAWMEM_LD8:
        case RAWMEM_LD16:
        case RAWMEM_LD32:
            *desc = extDesc(0, /*tosDelta=*/0);
            return true;

        case RAWMEM_ST8:
        case RAWMEM_ST16:
        case RAWMEM_ST32:
            *desc = extDesc(0, /*tosDelta=*/-1);
            return true;

        case RAWMEM_MEMMOVE:
            /* NEEDS_LR: the helper reach is a BLX, and the prologue's
             * decision to save lr is made from this flag. */
            *desc = extDesc(EXT_FLAG_NEEDS_LR, /*tosDelta=*/-3);
            return true;

        case RAWMEM_MEMCMP:
            *desc = extDesc(EXT_FLAG_NEEDS_LR, /*tosDelta=*/-3);
            return true;

        case RAWMEM_SLICECMP:
            *desc = extDesc(EXT_FLAG_NEEDS_LR, /*tosDelta=*/-4);
            return true;

        default:
            return false;
    }
}

/* Each op reserves its own AtomicBlock: its emitted halfwords have to stay
 * contiguous, and the size covers the core's service code too. */
extern "C" void extEmit(ExtSite &site)
{
    const uint8_t opcode = site.opcode();

    if(opcode == RAWMEM_SLICECMP)
    {
        Assembler::AtomicBlock atomic(site.a, /*poolEntries=*/1, /*extraBytes=*/56);
        emitSliceCmp(site);
    }
    else if(opcode == RAWMEM_MEMCMP)
    {
        Assembler::AtomicBlock atomic(site.a, /*poolEntries=*/1, /*extraBytes=*/48);
        emitMemcmp(site);
    }
    else if(opcode == RAWMEM_MEMMOVE)
    {
        Assembler::AtomicBlock atomic(site.a, /*poolEntries=*/1, /*extraBytes=*/32);
        emitMemmove(site);
    }
    else if(opcode >= RAWMEM_ST8)
    {
        Assembler::AtomicBlock atomic(site.a, /*poolEntries=*/1, /*extraBytes=*/20);
        emitStore(site, widthOf(opcode));
    }
    else
    {
        Assembler::AtomicBlock atomic(site.a, /*poolEntries=*/1, /*extraBytes=*/16);
        emitLoad(site, widthOf(opcode));
    }
}

/* No extHelperStackBytes here: everything is emitted inline, so this
 * extension needs none, and in the qemu image test_stack_budget.cpp owns
 * that symbol to drive its own accounting. */
