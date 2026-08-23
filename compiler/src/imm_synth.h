// jit-armv6m/compiler — 32-bit immediate synthesis. jit-armv6m/src/armv6.h
// (the shared C header) has no equivalent algorithm, only the raw encoders
// this builds on top of.
#ifndef JIT_ARMV6M_COMPILER_IMM_SYNTH_H_
#define JIT_ARMV6M_COMPILER_IMM_SYNTH_H_

#include <cstdint>

namespace jitc
{

class Emitter;

/** Materialize an arbitrary 32-bit constant into dst, MSB-first byte
 *  chunks (MOVS for the first nonzero byte, then LSLS #8; ADDS per
 *  remaining nonzero byte, skipping ADDS for a zero byte) — up to 7
 *  instructions, fewer for small values. */
void emitSynthesizeImm32(Emitter &e, uint32_t dstReg, uint32_t value);

/** How many halfwords emitSynthesizeImm32(_, dstReg, value) would emit —
 *  a pure function of value alone, needed by abi_strategy.cpp's own
 *  fixed-point resume-offset search so that search never needs a scratch
 *  emit-and-discard pass. Must stay in lockstep with emitSynthesizeImm32's
 *  own algorithm (cross-checked directly in test_imm_synth.cpp). */
uint32_t synthesizeImm32Length(uint32_t value);

constexpr bool fitsImm8(int32_t v)
{
    return v >= 0 && v <= 0xff;
}

constexpr bool fitsImm3(int32_t v)
{
    return v >= 0 && v <= 0x7;
}

} // namespace jitc

#endif // JIT_ARMV6M_COMPILER_IMM_SYNTH_H_
