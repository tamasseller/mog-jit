// jit-armv6m/compiler — what the condition flags hold right now. A statement
// about the processor rather than about any one value: an emitter establishes
// it, a consumer may read it instead of emitting a CMP of its own.
#ifndef JIT_ARMV6M_COMPILER_FLAGSTATE_H_
#define JIT_ARMV6M_COMPILER_FLAGSTATE_H_

#include <cstdint>
#include <cassert>

#include "armv6.h"

namespace jitc
{

class FlagState
{
    enum class Kind : uint8_t
    {
        Unknown,
        ZeroOf,  // N and Z hold what `CMP bits, #0` would leave
        Compare  // NZCV hold what a comparison left; `bits` is the condition it tested
    };

    Kind kind = Kind::Unknown;
    uint8_t bits = 0;

public:
    void forget() { kind = Kind::Unknown; }

    void zeroOf(uint32_t r) { kind = Kind::ZeroOf; bits = (uint8_t)r; }
    void compare(ArmV6M::Condition c) { kind = Kind::Compare; bits = (uint8_t)c; }

    /** True when N/Z already answer "is `r` zero", so a truthy test on a value
     *  living there needs no CMP of its own. */
    bool answersZeroOf(uint32_t r) const { return kind == Kind::ZeroOf && bits == (uint8_t)r; }

    bool isCompare() const { return kind == Kind::Compare; }

    ArmV6M::Condition cond() const
    {
        assert(kind == Kind::Compare); // GCOV_EXCL_LINE — a translator-logic bug
        return (ArmV6M::Condition)bits;
    }

    /** Any of `mask` was overwritten by something that left N/Z alone. A
     *  comparison's own result survives that: it describes a relation, not a
     *  register's current contents. */
    void clobberedAny(uint32_t mask)
    {
        if(kind == Kind::ZeroOf && (mask & (1u << bits)) != 0)
        {
            forget();
        }
    }
};

} // namespace jitc

#endif // JIT_ARMV6M_COMPILER_FLAGSTATE_H_
