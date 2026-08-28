// jit-armv6m/compiler — wire-format decode (isa-core.md §5). One
// instruction at a time from a raw byte stream, no heap, no pre-decoded
// array — translate_proc.cpp's own main loop (and every other
// per-procedure scan: needsLRSave, the last-argument-fold reference count,
// blocks.h's maxSpanBytes) decodes on demand through this.
//
// Instr is already flat and cheap to return by value, so one decodeInstr()
// serves every caller, skip-passes included.
#ifndef JIT_ARMV6M_COMPILER_DECODE_INSTR_H_
#define JIT_ARMV6M_COMPILER_DECODE_INSTR_H_

#include <cstdint>
#include "instr.h"
#include "ext.h"

namespace jitc
{

/** Unsigned LEB128 — mirrors bytecode.ts's decodeLeb128. Asserts rather
 *  than throwing on a truncated buffer (this target builds -fno-exceptions
 *  throughout, matching accstate.h's own convention) — a malformed/
 *  truncated program is a translator-input bug, never a legitimate runtime
 *  condition. */
uint32_t decodeLeb128(const uint8_t *bytes, uint32_t offset, uint32_t &next);

/** Bounded unsigned LEB128: the one an extension's own decode() must use
 *  (ext.h), since that runs from Runtime::init's walk on bytes nothing has
 *  validated yet. Returns false — touching nothing past bytes[bytesLen-1]
 *  — when the encoding starts at or past bytesLen, runs off the end still
 *  continuing, or is overlong for a u32. The unchecked variant above has
 *  no length parameter at all and its two guards are asserts, so it is not
 *  safe on untrusted bytes. */
bool decodeLeb128Checked(const uint8_t *bytes, uint32_t bytesLen, uint32_t offset,
    uint32_t &value, uint32_t &next);

/** The two boundaries in isa-core.md §5.1's byte space, which are NOT the
 *  same boundary:
 *
 *    0..123    assigned core opcodes (§5.2), the last being CONST #15.
 *    124..127  reserved for the CORE (§5.3) — four codes it hasn't assigned
 *              yet. Not extension space: an extension must never be offered
 *              one, or it would squat on the core's own range.
 *    >= 128    the extension range (§11), owned by the registered extension.
 *
 *  Runtime::init's directory walk rejects both non-core cases up front, with
 *  different reasons — a reserved core code means "this program wants a
 *  newer core", an unclaimed extension byte means "this image lacks that
 *  extension". That walk is the *only* gate: decodeInstr below asserts, and
 *  both the QEMU suite and fuzz/qemu_exec build -DNDEBUG, so without it a
 *  byte of 0x80 decoded as CONST 20 on real hardware and silently
 *  reinterpreted the rest of the stream. */
constexpr uint32_t LAST_CORE_OPCODE = 123;
constexpr uint32_t EXT_OPCODE_BASE = 128;

/** Ask the registered extension for the byte length of the extension
 *  instruction at bytes[offset..), filling `decl` with its packed
 *  declaration (ext.h). Returns 0 — meaning "reject the program" — when no
 *  extension is registered, its decode declines, or the length it claims
 *  makes no forward progress or runs past bytesLen. Those last two are
 *  checked here rather than trusted, so a bad extension produces
 *  RESOURCE_PROGRAM_EXT_UNKNOWN instead of a hang or an overrun. */
uint32_t extDecodeLength(const uint8_t *bytes, uint32_t bytesLen, uint32_t offset, uint32_t &decl,
    const ExtHooks *ext);

struct DecodedInstr
{
    Instr instr;
    uint32_t next; // byte offset just past this instruction
};

/** Decode the instruction at bytes[offset..). Only ever called on bytes
 *  Runtime::init's walk already accepted, so a code past LAST_CORE_OPCODE
 *  is a translator-logic bug by then, not untrusted input — hence an
 *  assert, defence in depth behind that walk. */
DecodedInstr decodeInstr(const uint8_t *bytes, uint32_t bytesLen, uint32_t offset,
    const ExtHooks *ext = nullptr);

} // namespace jitc

#endif // JIT_ARMV6M_COMPILER_DECODE_INSTR_H_
