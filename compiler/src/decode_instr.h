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

constexpr uint32_t LAST_CORE_OPCODE = 123;
constexpr uint32_t EXT_OPCODE_BASE = 128;

uint32_t extDecodeLength(const uint8_t *bytes, uint32_t bytesLen, uint32_t offset, uint32_t &decl,
    const ExtHooks *ext);

struct DecodedInstr
{
    Instr instr;
    uint32_t next; // byte offset just past this instruction
};

DecodedInstr decodeInstr(const uint8_t *bytes, uint32_t bytesLen, uint32_t offset,
    const ExtHooks *ext = nullptr);

} // namespace jitc

#endif // JIT_ARMV6M_COMPILER_DECODE_INSTR_H_
