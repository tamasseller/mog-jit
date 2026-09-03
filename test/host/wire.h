#ifndef JIT_ARMV6M_TEST_HOST_WIRE_H_
#define JIT_ARMV6M_TEST_HOST_WIRE_H_

#include <cstdint>

#include "bytecode_default.h"
#include "proc_scan.h"
#include "decode_instr.h"

/* Every fixture in these tests builds its bytes in host memory, so the
 * mapped accessor is what serves them — one place to say so. */

inline BcReader wireOver(const uint8_t *bytes, uint32_t len)
{
    BcReader r;
    r.open(bcMapped(bytes), len);
    return r;
}

inline jitc::BodyScanResult scanBytes(const uint8_t *bytes, uint32_t len, uint32_t stackFloor = 0)
{
    BcReader r = wireOver(bytes, len);
    return jitc::scanProcBody(r, stackFloor);
}

/** A reader over an encodeProgram() blob, left past proc_count — exactly
 *  where Runtime::loadProgram picks up. */
inline BcReader wireAtBodies(const uint8_t *bytes, uint32_t len)
{
    BcReader r = wireOver(bytes, len);

    uint32_t procCount = 0;
    jitc::decodeLeb128(r, procCount);

    return r;
}

/* What decodeInstr used to return: the instruction plus how far the wire
 * moved, which is what pins an operand shape. */
struct WireInstr
{
    jitc::Instr instr;
    uint32_t consumed;
    bool ok;
};

inline WireInstr decodeFrom(BcReader &r)
{
    const uint32_t before = r.remaining();

    WireInstr d{};
    d.ok = jitc::decodeInstr(r.next(), r, d.instr);
    d.consumed = before - r.remaining();

    return d;
}

inline WireInstr decodeOne(const uint8_t *bytes, uint32_t len)
{
    BcReader r = wireOver(bytes, len);
    return decodeFrom(r);
}

#endif // JIT_ARMV6M_TEST_HOST_WIRE_H_
