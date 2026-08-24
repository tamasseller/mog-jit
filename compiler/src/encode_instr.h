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

/** One procedure, pre-encoding — the Instr[] literal a fixture is authored
 *  as, plus its own arg_count. Mirrors packages/machine/src/rtl.ts's
 *  RtlProc, not jitc::Proc (proc.h): a Proc's own body is already-encoded
 *  wire bytes, exactly what encodeProgram below produces, never what it
 *  consumes. */
struct ProcSource
{
    uint32_t argCount;
    const Instr *body;
    uint32_t bodyCount; // Instr entries, not bytes
};

/** Encodes a whole program (isa-core.md §5.5): proc_count:LEB128, then
 *  each procedure's own arg_count:LEB128 immediately followed by its own
 *  body — no header table, no stored body length. Mirrors
 *  packages/machine/src/bytecode.ts's encodeProgram. Returns the total
 *  byte length written. */
uint32_t encodeProgram(const ProcSource *procs, uint32_t procCount, uint8_t *out, uint32_t outCapacity);

/** jit-armv6m's own wire envelope (docs/design.md §2's static stack-
 *  overflow guarantee) — max_call_depth:LEB128 total_depth:LEB128
 *  prepended to an ordinary encodeProgram blob, mirroring
 *  packages/machine/src/bytecode.ts's encodeJitProgram. Unlike that
 *  side, there is no validateProgram here to compute the two stats from
 *  the body — the caller supplies them directly, hand-derived from the
 *  program's own known shape (this file's own callers are all test
 *  fixtures with a small, by-construction-known call/stack depth, the
 *  same reasoning test/qemu/main.cpp's own header comment already gives
 *  for its enterProgramOnStack/enterProgramSplit scenarios). Returns the
 *  total byte length written. */
uint32_t encodeJitProgram(uint32_t maxCallDepth, uint32_t totalDepth, const ProcSource *procs, uint32_t procCount, uint8_t *out, uint32_t outCapacity);

} // namespace jitc

#endif // JIT_ARMV6M_COMPILER_ENCODE_INSTR_H_
