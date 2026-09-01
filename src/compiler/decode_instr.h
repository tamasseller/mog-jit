#ifndef JIT_ARMV6M_COMPILER_DECODE_INSTR_H_
#define JIT_ARMV6M_COMPILER_DECODE_INSTR_H_

#include <cstdint>
#include "instr.h"
#include "ext.h"

namespace jitc
{

uint32_t decodeLeb128(const uint8_t *bytes, uint32_t offset, uint32_t &next);

bool decodeLeb128Checked(const uint8_t *bytes, uint32_t bytesLen, uint32_t offset,
    uint32_t &value, uint32_t &next);

constexpr uint32_t LAST_CORE_OPCODE = 127;
constexpr uint32_t EXT_OPCODE_BASE = 128;

/** `CONST #0..#15`'s first code — a small immediate is `code - this`. */
constexpr uint32_t SMALL_CONST_BASE = 109;

/** §5.2's last three codes: `MISC_CF`, `MISC_UNARY`, `MISC_BINARY`. Each takes
 *  a sub-code as its trailing LEB128 operand (§5.3). */
constexpr uint32_t MISC_BASE = 125;
constexpr uint32_t MISC_UNARY = 126;

/** Whether `code`'s sub-code names anything yet. An unassigned one has no
 *  defined operand shape, so a walk must reject it rather than skip it. */
bool miscSubCodeAssigned(uint32_t code, uint32_t sub);

uint32_t extDecodeLength(const uint8_t *bytes, uint32_t bytesLen, uint32_t offset, uint32_t &decl);

struct DecodedInstr
{
    Instr instr;
    uint32_t next; // byte offset just past this instruction
};

DecodedInstr decodeInstr(const uint8_t *bytes, uint32_t bytesLen, uint32_t offset);

} // namespace jitc

#endif // JIT_ARMV6M_COMPILER_DECODE_INSTR_H_
