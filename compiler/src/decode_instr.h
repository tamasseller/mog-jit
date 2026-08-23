// jit-armv6m/compiler — wire-format decode, ported from
// jit-armv6m/prototype/src/bytecodeReader.ts's decodeInstr (isa-core.md
// §5). One instruction at a time from a raw byte stream, no heap, no
// pre-decoded array — translate_proc.cpp's own main loop (and every other
// per-procedure scan: needsLRSave, the last-argument-fold reference count,
// blocks.h's maxSpanBytes) decodes on demand through this, exactly as
// bytecodeReader.ts's own header describes as "a no-heap native port
// needs this exact table again, in C++, regardless."
//
// Unlike bytecodeReader.ts, there is no separate lean readInstr()/
// InstrKind here — native's Instr struct is already flat and cheap to
// return by value (no generic RtlInstr shape to avoid materializing), so
// one decodeInstr() serves every caller, skip-passes included.
#ifndef JIT_ARMV6M_COMPILER_DECODE_INSTR_H_
#define JIT_ARMV6M_COMPILER_DECODE_INSTR_H_

#include <cstdint>
#include "instr.h"

namespace jitc {

/** Unsigned LEB128 — mirrors bytecode.ts's decodeLeb128/bytecodeReader.ts's
 *  readLeb128. Asserts rather than throwing on a truncated buffer (this
 *  target builds -fno-exceptions throughout, matching accstate.h's own
 *  convention) — a malformed/truncated program is a translator-input bug,
 *  never a legitimate runtime condition. */
uint32_t decodeLeb128(const uint8_t *bytes, uint32_t offset, uint32_t &next);

struct DecodedInstr {
    Instr instr;
    uint32_t next; // byte offset just past this instruction
};

/** Decode the instruction at bytes[offset..) — asserts on a byte >= 128
 *  (an extension opcode; this JIT never registers one, isa-core.md §11)
 *  or a reserved/unassigned code (isa-core.md §5.3), exactly like
 *  decodeInstr's own two throws on the TS side. */
DecodedInstr decodeInstr(const uint8_t *bytes, uint32_t bytesLen, uint32_t offset);

} // namespace jitc

#endif // JIT_ARMV6M_COMPILER_DECODE_INSTR_H_
