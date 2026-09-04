#ifndef JIT_ARMV6M_COMPILER_DECODE_INSTR_H_
#define JIT_ARMV6M_COMPILER_DECODE_INSTR_H_

#include <cstdint>
#include "instr.h"
#include "bytecode.h"

namespace jitc
{

/** False for a LEB128 overlong for a u32, or one the body ends in the middle of. */
bool decodeLeb128(BcReader &r, uint32_t &value);

constexpr uint32_t LAST_CORE_OPCODE = 127;
constexpr uint32_t EXT_OPCODE_BASE = 128;

/** `CONST #0..#15`'s first code — a small immediate is `code - this`. */
constexpr uint32_t SMALL_CONST_BASE = 109;

/** `BLOCK_END`'s own code (§5.2). */
constexpr uint32_t BLOCK_END_OPCODE = 96;

/** §5.2's last three codes: `MISC_CF`, `MISC_UNARY`, `MISC_BINARY`. Each takes
 *  a sub-code as its trailing LEB128 operand (§5.3). */
constexpr uint32_t MISC_BASE = 125;
constexpr uint32_t MISC_UNARY = 126;

/** Whether `code`'s sub-code names anything yet. An unassigned one has no
 *  defined operand shape, so a walk must reject it rather than skip it. */
bool miscSubCodeAssigned(uint32_t code, uint32_t sub);

/** `code` is the opcode byte already read off `r`, whose operands follow it
 *  there. False for an encoding a walk must reject rather than step over: a
 *  §5.3 escape naming an unassigned sub-code, or an overlong LEB128.
 *
 *  An extension opcode yields `Op::EXT` with its operands still unread — only
 *  the extension knows how many there are, and only it may consume them
 *  (ext.h). */
bool decodeInstr(uint8_t code, BcReader &r, Instr &out);

} // namespace jitc

#endif // JIT_ARMV6M_COMPILER_DECODE_INSTR_H_
