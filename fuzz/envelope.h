#ifndef JIT_ARMV6M_FUZZ_ENVELOPE_H_
#define JIT_ARMV6M_FUZZ_ENVELOPE_H_

#include <cstdint>

#include "bytecode_default.h"
#include "decode_instr.h"

/* Every driver here holds its program in host memory and has to read the
 * envelope header itself, since Executor::run is not what it drives. */

struct Envelope
{
    uint32_t maxCallDepth;
    uint32_t totalDepth;
    uint32_t procCount;
    uint32_t bodyOffset;
};

inline Envelope readEnvelope(const uint8_t *data, uint32_t len)
{
    BcReader r;
    r.open(bcMapped(data), len);

    Envelope e{};
    jitc::decodeLeb128(r, e.maxCallDepth);
    jitc::decodeLeb128(r, e.totalDepth);
    jitc::decodeLeb128(r, e.procCount);
    e.bodyOffset = len - r.remaining();

    return e;
}

/** Positioned on the first procedure's arg_count, for Runtime::loadProgram. */
inline BcReader wireAtBodies(const uint8_t *data, uint32_t len, uint32_t bodyOffset)
{
    BcReader r;
    r.open(bcMapped(data + bodyOffset), len - bodyOffset);
    return r;
}

#endif // JIT_ARMV6M_FUZZ_ENVELOPE_H_
