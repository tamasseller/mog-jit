/* The sample-stream extension, target half — the mirror of
 * bench/sampstream_ext.ts, which carries the specification.
 *
 * Every op emits inline. That is not a convenience: the suite exists to
 * compare emitted Thumb against C doing the same work, and an op reaching
 * its data through ExtSite::cHelperCall would spend a dozen instructions on
 * the seam that the C side spends none on, so the numbers would describe
 * the seam rather than the JIT.
 *
 * All three buffers are static, so their addresses are link-time constants
 * and nothing here needs per-excursion extension state (design.md §18.1):
 * every site materializes its base as a pooled literal.
 *
 * Indices are masked to the buffer size and scaled to the element width in
 * two shifts — mask and scale fold together, unlike ext_rawmem's
 * maskAndAlign, which needs a third shift because it aligns *down* from an
 * arbitrary byte offset. Nothing can leave a buffer and nothing can be
 * unaligned, which is the whole safety argument and why the emitted code
 * carries no bounds check. */

#include "ext_sampstream.h"

#include "ext.h"
#include "assembler.h"
#include "registers.h"
#include "armv6.h"

using namespace jitc;

using R = ArmV6M::LoReg;

int16_t g_sampOut[SAMP_OUT_SAMPLES];
SampEvents g_sampEvents;

void sampStreamReset()
{
    for(uint32_t i = 0; i < SAMP_OUT_SAMPLES; i++) g_sampOut[i] = 0;
    for(uint32_t i = 0; i < SAMP_EVENTS; i++) g_sampEvents.entries[i] = 0;
    g_sampEvents.count = 0;
}

namespace
{
constexpr uint32_t OFF_REG = 1;  // r1
constexpr uint32_t VAL_REG = 2;  // r2
constexpr uint32_t BASE_REG = 3; // r3

/* (value & ((1 << bits) - 1)) << scale, in two shifts and no scratch
 * register. `scale` never exceeds `bits`, so the right shift amount stays
 * non-negative. */
void maskAndScale(Assembler &a, uint32_t reg, uint32_t bits, uint32_t scale)
{
    const uint32_t drop = 32 - bits;
    const R r((uint16_t)reg);

    a.emit(ArmV6M::lsls(r, r, ArmV6M::Imm<5>((uint16_t)drop)));
    a.emit(ArmV6M::lsrs(r, r, ArmV6M::Imm<5>((uint16_t)(drop - scale))));
}

/* acc = in[acc], a unary transform on the accumulator like NEG or CLZ: the
 * index arrives in acc and the sign-extended sample replaces it, touching
 * the operand stack not at all. */
void emitSampleAt(ExtSite &site)
{
    site.accInto(ACC_REG);
    maskAndScale(site.a, ACC_REG, SAMP_IN_BITS, 1);
    site.a.materializeImm32(BASE_REG, (uint32_t)(uintptr_t)g_sampIn);

    site.a.emit(ArmV6M::ldrsh(R((uint16_t)ACC_REG), R((uint16_t)BASE_REG), R((uint16_t)ACC_REG)));

    site.accIsNowIn(ACC_REG);
}

/* out[pop()] = acc, mirroring the core's own STORE: acc carries the value
 * and only the index comes off the stack. acc is left alone — OUT_AT
 * declares no writesAcc, so the value stays readable after the write. */
void emitOutAt(ExtSite &site)
{
    site.pop(OFF_REG);
    site.accInto(VAL_REG);
    maskAndScale(site.a, OFF_REG, SAMP_OUT_BITS, 1);
    site.a.materializeImm32(BASE_REG, (uint32_t)(uintptr_t)g_sampOut);

    site.a.emit(ArmV6M::strh(R((uint16_t)VAL_REG), R((uint16_t)BASE_REG), R((uint16_t)OFF_REG)));
}

/* events.entries[count++ & MASK] = (acc << 4) | kind.
 *
 * acc holds the sample index and must survive: TRIGGER declares readsAcc
 * and no writesAcc, so r0 is off limits and the packed word is built in a
 * scratch. The count is read once, written back incremented, and separately
 * masked into the entry offset — the pre-increment value is what indexes
 * the ring, so it needs a second live copy. */
void emitTrigger(ExtSite &site, uint32_t kind)
{
    Assembler &a = site.a;

    const R acc((uint16_t)ACC_REG), packed((uint16_t)OFF_REG);
    const R count((uint16_t)VAL_REG), base((uint16_t)BASE_REG);

    site.accInto(ACC_REG);
    a.materializeImm32(BASE_REG, (uint32_t)(uintptr_t)&g_sampEvents);

    a.emit(ArmV6M::ldr(count, base, ArmV6M::Uoff<2, 5>(0)));

    a.emit(ArmV6M::mov(ArmV6M::AnyReg(OFF_REG), ArmV6M::AnyReg(VAL_REG)));
    a.emit(ArmV6M::adds(packed, ArmV6M::Imm<8>(1)));
    a.emit(ArmV6M::str(packed, base, ArmV6M::Uoff<2, 5>(0)));

    maskAndScale(a, VAL_REG, SAMP_EVENT_BITS, 2);
    a.emit(ArmV6M::adds(count, ArmV6M::Imm<8>(sizeof(uint32_t)))); // past the count word

    a.emit(ArmV6M::lsls(packed, acc, ArmV6M::Imm<5>((uint16_t)TRIGGER_KIND_BITS)));

    if(kind != 0)
    {
        a.emit(ArmV6M::adds(packed, ArmV6M::Imm<8>((uint16_t)kind)));
    }

    a.emit(ArmV6M::str(packed, base, count));
}

/* The kind is a LEB128 operand, and a benchmark never needs more than the
 * one byte TRIGGER_MAX_KIND fits in — a longer field is a malformed
 * program, reported as an undecodable opcode. */
uint32_t decodeKind(const uint8_t *bytes, uint32_t bytesLen, uint32_t offset, uint32_t *kind)
{
    if(offset + 1 >= bytesLen) return 0;

    const uint8_t b = bytes[offset + 1];
    if(b > TRIGGER_MAX_KIND) return 0;

    *kind = b;
    return 2;
}
} // namespace

extern "C" uint32_t extDecode(const uint8_t *bytes, uint32_t bytesLen, uint32_t offset, uint32_t *decl)
{
    const uint32_t flags = EXT_FLAG_ATOMIC;

    switch(bytes[offset])
    {
        case SAMPSTREAM_SAMPLE_AT:
            *decl = extDecl(flags, /*tosDelta=*/0, /*halfwords=*/8, /*poolWords=*/1);
            return 1;

        case SAMPSTREAM_OUT_AT:
            *decl = extDecl(flags, /*tosDelta=*/-1, /*halfwords=*/10, /*poolWords=*/1);
            return 1;

        case SAMPSTREAM_TRIGGER:
        {
            uint32_t kind = 0;
            const uint32_t len = decodeKind(bytes, bytesLen, offset, &kind);
            if(len == 0) return 0;

            *decl = extDecl(flags, /*tosDelta=*/0, /*halfwords=*/18, /*poolWords=*/1);
            return len;
        }

        default:
            return 0;
    }
}

extern "C" void extEmit(ExtSite &site)
{
    const uint8_t opcode = *site.opcode();

    if(opcode == SAMPSTREAM_SAMPLE_AT)
    {
        emitSampleAt(site);
    }
    else if(opcode == SAMPSTREAM_OUT_AT)
    {
        emitOutAt(site);
    }
    else
    {
        emitTrigger(site, *site.operands());
    }
}

/* Everything is emitted inline, so no helper frame is reached from a
 * translation and the up-front reservation needs nothing extra. */
extern "C" uint32_t extHelperStackBytes()
{
    return 0;
}
