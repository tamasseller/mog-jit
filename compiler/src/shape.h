// jit-armv6m/compiler — a value's location right now, ported from
// jit-armv6m/prototype/src/shape.ts. Two plain fields with a discriminant
// instead of a tagged union — same information, no ambiguity.
#ifndef JIT_ARMV6M_COMPILER_SHAPE_H_
#define JIT_ARMV6M_COMPILER_SHAPE_H_

#include <cstdint>

namespace jitc {

class Emitter;

struct Shape {
    bool isImm = true;
    int32_t imm = 0; // valid iff isImm
    uint32_t reg = 0; // valid iff !isImm

    static constexpr Shape ofImm(int32_t v) { return Shape{true, v, 0}; }
    static constexpr Shape ofReg(uint32_t r) { return Shape{false, 0, r}; }
};

/** Turn any Shape into a concrete value sitting in dstReg — a no-op when
 *  shape is already dstReg itself. */
void materializeShape(Emitter &e, const Shape &shape, uint32_t dstReg);

/** A Shape as a register, materializing into scratchReg only if it isn't
 *  one already. */
uint32_t shapeToReg(Emitter &e, const Shape &shape, uint32_t scratchReg);

} // namespace jitc

#endif // JIT_ARMV6M_COMPILER_SHAPE_H_
