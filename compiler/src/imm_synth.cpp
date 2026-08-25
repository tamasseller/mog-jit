#include "imm_synth.h"
#include "emitter.h"
#include "armv6.h"

namespace jitc
{

using R = ArmV6M::LoReg;

/** MSB-first byte decomposition, and the index of the first nonzero byte
 *  among bytes[0..2] (bytes[3] is never skipped) — shared by
 *  emitSynthesizeImm32 and synthesizeImm32Length so the two can never
 *  silently drift apart in *how* they decompose, only accidentally in
 *  what they do with it (guarded by test_imm_synth.cpp's own cross-check). */
struct Decomposed
{
    uint8_t bytes[4];
    int start;
};

static Decomposed decompose(uint32_t v)
{
    Decomposed d;
    d.bytes[0] = (uint8_t)((v >> 24) & 0xff);
    d.bytes[1] = (uint8_t)((v >> 16) & 0xff);
    d.bytes[2] = (uint8_t)((v >> 8) & 0xff);
    d.bytes[3] = (uint8_t)(v & 0xff);
    d.start = 0;
    while(d.start < 3 && d.bytes[d.start] == 0)
    {
        d.start++;
    }
    return d;
}

void emitSynthesizeImm32(Emitter &e, uint32_t dstReg, uint32_t value)
{
    Decomposed d = decompose(value);
    e.emit(ArmV6M::movs(R((uint16_t)dstReg), ArmV6M::Imm<8>(d.bytes[d.start])));
    for(int i = d.start + 1; i < 4; i++)
    {
        e.emit(ArmV6M::lsls(R((uint16_t)dstReg), R((uint16_t)dstReg), ArmV6M::Imm<5>(8)));
        if(d.bytes[i] != 0)
        {
            e.emit(ArmV6M::adds(R((uint16_t)dstReg), ArmV6M::Imm<8>(d.bytes[i])));
        }
    }
}

uint32_t synthesizeImm32Length(uint32_t value)
{
    Decomposed d = decompose(value);
    uint32_t count = 1; // movs
    for(int i = d.start + 1; i < 4; i++)
    {
        count += 1; // lsls
        if(d.bytes[i] != 0)
        {
            count += 1; // adds
        }
    }
    return count;
}

} // namespace jitc
