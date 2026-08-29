// jit-armv6m/compiler — wire-format encode, ported from
// packages/machine/src/bytecode.ts's encodeInstr/encodeBody/encodeLeb128
// (isa-core.md §5). The JIT itself never calls this — it only ever
// decodes (decode_instr.h) — but a fixture is still authored as a plain
// Instr[] literal (matching rtl.ts's own constructors 1:1, instr.h's own
// header) and needs turning into bytes exactly once, at test setup, before
// it can reach translateProc's now byte-stream-only Proc::body. Kept in
// compiler/src rather than compiler/test since it's a genuine wire codec,
// not test-only logic, the same way bytecode.ts's own encodeInstr sits
// alongside its decodeInstr in @ppl/machine rather than in a test helper.
#ifndef JIT_ARMV6M_COMPILER_ENCODE_INSTR_H_
#define JIT_ARMV6M_COMPILER_ENCODE_INSTR_H_

#include <cstdint>
#include "instr.h"

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

} // namespace jitc

#endif // JIT_ARMV6M_COMPILER_ENCODE_INSTR_H_
