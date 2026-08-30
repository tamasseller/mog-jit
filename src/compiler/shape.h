// jit-armv6m/compiler — a value's location right now. Two plain fields
// with a discriminant instead of a tagged union — same information, no
// ambiguity.
#ifndef JIT_ARMV6M_COMPILER_SHAPE_H_
#define JIT_ARMV6M_COMPILER_SHAPE_H_

#include <cstdint>

namespace jitc
{

class Assembler;

struct Shape
{
    bool isImm = true;

    union 
    {
        int32_t imm = 0;
        uint32_t reg;   
    };

    static constexpr Shape ofImm(int32_t v)
    {
        return Shape{.isImm = true, .imm = v};
    }

    static constexpr Shape ofReg(uint32_t r)
    {
        return Shape{.isImm = false, .reg = r};
    }

    void materialize(Assembler &a, uint32_t dstReg) const;
    uint32_t peek(Assembler &a, uint32_t scratchReg) const;
};

} // namespace jitc

#endif // JIT_ARMV6M_COMPILER_SHAPE_H_
