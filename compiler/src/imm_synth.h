// jit-armv6m/compiler — instruction-field immediate fit checks. The
// synthesis/pooling machinery these used to sit alongside (emitSynthesizeImm32,
// synthesizeImm32Length, isPoolingEligible) moved into assembler.h/.cpp —
// these two are pure instruction-encoding predicates (does this value fit
// the imm3/imm8 field of a native Thumb form?), unrelated to whether a
// value is worth pooling, and stay needed wherever a caller picks between a
// native immediate form and materializing into a register first
// (binops.cpp, abi_strategy.cpp).
#ifndef JIT_ARMV6M_COMPILER_IMM_SYNTH_H_
#define JIT_ARMV6M_COMPILER_IMM_SYNTH_H_

#include <cstdint>

namespace jitc
{

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
