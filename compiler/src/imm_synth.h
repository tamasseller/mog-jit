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

/** Shortest synthesis worth replacing with a pooled `LDR Rd,[pc,#imm]`
 *  (2 bytes of instruction + a 4-byte pool word). At 3 the two tie on
 *  size, but a mid-procedure pool's branch-around is *executed*, costing
 *  a chunk of n sites 2n+3 cycles against 3n — a loss for small n. From 4
 *  up, pooling wins on both size and cycles. */
constexpr uint32_t POOLING_MIN_LENGTH = 4;

/** Whether value is worth pooling rather than synthesizing inline.
 *  Deliberately expressed in terms of synthesizeImm32Length so there's no
 *  second cost model to keep in lockstep with the real one. */
inline bool isPoolingEligible(uint32_t value)
{
    return synthesizeImm32Length(value) >= POOLING_MIN_LENGTH;
}

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
