// jit-armv6m/compiler — where a value is right now: in a register, or still an
// unmaterialized literal. Both are legal wherever a Shape is accepted.
#ifndef JIT_ARMV6M_COMPILER_SHAPE_H_
#define JIT_ARMV6M_COMPILER_SHAPE_H_

#include <cstdint>
#include <cassert>

#include "armv6.h"
#include "effect.h"

namespace jitc
{

class Assembler;

class Shape
{
    enum class Kind : uint8_t
    {
        Imm,
        Reg
    };

    constexpr Shape(Kind k, uint32_t v): kind(k), bits(v) {}

    Kind kind;
    uint32_t bits;

public:
    static constexpr Shape ofImm(int32_t v) { return Shape(Kind::Imm, (uint32_t)v); }
    static constexpr Shape ofReg(uint32_t r) { return Shape(Kind::Reg, r); }

    bool isImm() const { return kind == Kind::Imm; }
    bool isReg() const { return kind == Kind::Reg; }

    int32_t imm() const
    {
        assert(kind == Kind::Imm); // GCOV_EXCL_LINE — a translator-logic bug, never legitimate input
        return (int32_t)bits;
    }

    uint32_t reg() const
    {
        assert(kind == Kind::Reg); // GCOV_EXCL_LINE — a translator-logic bug, never legitimate input
        return bits;
    }


    /** Puts the value in `dstReg`. */
    Effect materialize(Assembler &a, uint32_t dstReg) const;

    /** A register the value can be read from — `scratchReg` only if one must be made. */
    uint32_t sourceReg(Assembler &a, uint32_t scratchReg) const;
};

} // namespace jitc

#endif // JIT_ARMV6M_COMPILER_SHAPE_H_
