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

namespace jitc {

/** Appends n's unsigned LEB128 encoding to out[outLen..), advancing
 *  outLen. Asserts if out is too small to hold it (outCapacity) — a
 *  fixture-authoring bug, never a runtime condition on this path. */
void encodeLeb128(uint32_t n, uint8_t *out, uint32_t &outLen, uint32_t outCapacity);

/** Appends instr's own wire encoding to out[outLen..), advancing outLen.
 *  Asserts (same reasoning as encodeLeb128) if instr isn't representable
 *  (e.g. a comparison with a REG_REG/PEEK_PEEK combo — isa-core.md §4.2
 *  has no such form) or out overflows. */
void encodeInstr(const Instr &instr, uint8_t *out, uint32_t &outLen, uint32_t outCapacity);

/** Encodes count instructions back to back — one procedure's own body,
 *  no header. Returns the total byte length written. */
uint32_t encodeBody(const Instr *body, uint32_t count, uint8_t *out, uint32_t outCapacity);

} // namespace jitc

#endif // JIT_ARMV6M_COMPILER_ENCODE_INSTR_H_
