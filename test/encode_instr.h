// Wire-format encode (isa-core.md §5), the inverse of the JIT's own
// decode_instr.h. Only test and fuzz code needs it: the JIT decodes,
// never encodes, but fixtures are authored as Instr[] literals and must
// be turned into bytes once at setup.
#ifndef JIT_ARMV6M_TEST_ENCODE_INSTR_H_
#define JIT_ARMV6M_TEST_ENCODE_INSTR_H_

#include <cstdint>
#include "instr.h"
#include "program_frame.h"

namespace jitc
{

void encodeLeb128(uint32_t n, uint8_t *out, uint32_t &outLen, uint32_t outCapacity);
void encodeInstr(const Instr &instr, uint8_t *out, uint32_t &outLen, uint32_t outCapacity);
uint32_t encodeBody(const Instr *body, uint32_t count, uint8_t *out, uint32_t outCapacity);

struct ProcSource
{
    uint32_t argCount;
    const Instr *body;
    uint32_t bodyCount; // Instr entries, not bytes
};

uint32_t encodeProgram(const ProcSource *procs, uint32_t procCount, uint8_t *out, uint32_t outCapacity);
uint32_t encodeJitProgram(uint32_t maxCallDepth, uint32_t totalDepth, const ProcSource *procs, uint32_t procCount, uint8_t *out, uint32_t outCapacity);

/* For the handful of TESTs that spell a program out byte by byte rather than
 * going through encodeJitProgram: returns the framed length. */
uint32_t appendProgramFrame(uint8_t *out, uint32_t len, uint32_t outCapacity);

/* Those literals are deliberately malformed, so no encoder would produce them
 * — this is what puts a valid frame on one anyway. */
struct FramedProgram
{
    uint8_t bytes[16];
    uint32_t len;
};

FramedProgram framedProgram(const uint8_t *literal, uint32_t len);

} // namespace jitc

#endif // JIT_ARMV6M_TEST_ENCODE_INSTR_H_
