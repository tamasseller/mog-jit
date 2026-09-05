#include "encode_instr.h"

#include <cassert>

namespace jitc
{

static bool isArithOp(Op op)
{
    return op >= Op::ADD && op <= Op::ASR;
}

static void putByte(uint8_t b, uint8_t *out, uint32_t &outLen, uint32_t outCapacity)
{
    assert(outLen < outCapacity); // GCOV_EXCL_LINE — fixture-authoring bug, never a runtime condition
    out[outLen++] = b;
}

void encodeLeb128(uint32_t n, uint8_t *out, uint32_t &outLen, uint32_t outCapacity)
{
    uint32_t rest = n;
    do
    {
        uint8_t byte = (uint8_t)(rest & 0x7f);
        rest >>= 7;
        putByte(rest != 0 ? (uint8_t)(byte | 0x80) : byte, out, outLen, outCapacity);
    } while(rest != 0);
}

void encodeInstr(const Instr &instr, uint8_t *out, uint32_t &outLen, uint32_t outCapacity)
{
    Op op = instr.op;

    if(isArithOp(op))
    {
        uint32_t arithIdx = (uint32_t)op - (uint32_t)Op::ADD;
        switch(instr.combo)
        {
        case Combo::REG_ACC:
            putByte((uint8_t)(arithIdx * 5 + 0), out, outLen, outCapacity);
            encodeLeb128(instr.target, out, outLen, outCapacity);
            return;
        case Combo::REG_REG:
            putByte((uint8_t)(arithIdx * 5 + 1), out, outLen, outCapacity);
            encodeLeb128(instr.target, out, outLen, outCapacity);
            return;
        case Combo::PEEK_PEEK:
            putByte((uint8_t)(arithIdx * 5 + 2), out, outLen, outCapacity);
            return;
        case Combo::POP_ACC:
            putByte((uint8_t)(arithIdx * 5 + 3), out, outLen, outCapacity);
            return;
        case Combo::IMM_ACC:
            putByte((uint8_t)(arithIdx * 5 + 4), out, outLen, outCapacity);
            encodeLeb128((uint32_t)instr.imm, out, outLen, outCapacity);
            return;
        default:
            assert(false && "encode_instr: arithmetic op with no combo"); // GCOV_EXCL_LINE
            return; // GCOV_EXCL_LINE
        }
    }

    if(isComparisonOp(op))
    {
        uint32_t cmpIdx = (uint32_t)op - (uint32_t)Op::EQ;
        switch(instr.combo)
        {
        case Combo::REG_ACC:
            putByte((uint8_t)(50 + cmpIdx * 4 + 0), out, outLen, outCapacity);
            encodeLeb128(instr.target, out, outLen, outCapacity);
            return;
        case Combo::POP_ACC:
            putByte((uint8_t)(50 + cmpIdx * 4 + 1), out, outLen, outCapacity);
            return;
        case Combo::IMM_ACC:
            if(instr.imm == 0)
            {
                putByte((uint8_t)(50 + cmpIdx * 4 + 2), out, outLen, outCapacity);
                return;
            }
            putByte((uint8_t)(50 + cmpIdx * 4 + 3), out, outLen, outCapacity);
            encodeLeb128((uint32_t)instr.imm, out, outLen, outCapacity);
            return;
        case Combo::REG_REG:
        case Combo::PEEK_PEEK: // GCOV_EXCL_LINE — isa-core.md §4.2: comparisons have no REG_REG/PEEK_PEEK combo, structurally unreachable
            assert(false && "encode_instr: comparison has no REG_REG/PEEK_PEEK combo (isa-core.md §4.2)"); // GCOV_EXCL_LINE
            return; // GCOV_EXCL_LINE
        default:
            assert(false && "encode_instr: comparison op with no combo"); // GCOV_EXCL_LINE
            return; // GCOV_EXCL_LINE
        }
    }

    if(op >= Op::NEG && op <= Op::UXTH)
    {
        putByte((uint8_t)(90 + ((uint32_t)op - (uint32_t)Op::NEG)), out, outLen, outCapacity);
        return;
    }

    if(op == Op::REVBITS || op == Op::CLZ)
    {
        putByte(126, out, outLen, outCapacity); // MISC_UNARY (isa-core.md §5.3)
        encodeLeb128(op == Op::REVBITS ? 0 : 1, out, outLen, outCapacity);
        return;
    }

    switch(op)
    {
    case Op::BLOCK_END:
        putByte(96, out, outLen, outCapacity);
        return;
    case Op::LOOP_PRE:
        putByte(97, out, outLen, outCapacity);
        return;
    case Op::LOOP_POST:
        putByte(98, out, outLen, outCapacity);
        return;
    case Op::FALLTHROUGH:
        putByte(127, out, outLen, outCapacity); // MISC_OTHER (isa-core.md §5.3)
        encodeLeb128(0, out, outLen, outCapacity);
        return;
    case Op::DEFAULT:
        putByte(127, out, outLen, outCapacity);
        encodeLeb128(1, out, outLen, outCapacity);
        return;
    case Op::DROP:
        putByte(127, out, outLen, outCapacity);
        /* §5.4: `#1..#4` have their own sub-codes, the rest is biased by 5. */
        if(instr.imm >= 1 && instr.imm <= 4)
        {
            encodeLeb128(2 + (uint32_t)instr.imm, out, outLen, outCapacity);
            return;
        }
        encodeLeb128(2, out, outLen, outCapacity);
        encodeLeb128((uint32_t)instr.imm - 5, out, outLen, outCapacity);
        return;
    case Op::BR_TABLE:
        if(instr.imm == 1)
        {
            putByte(99, out, outLen, outCapacity);
            return;
        }
        /* §5.4: the extended operand is biased by 2. */
        putByte(100, out, outLen, outCapacity);
        encodeLeb128((uint32_t)instr.imm - 2, out, outLen, outCapacity);
        return;
    case Op::CALL:
        putByte(101, out, outLen, outCapacity);
        encodeLeb128(instr.calleeIndex, out, outLen, outCapacity);
        return;
    case Op::RETURN:
        putByte(102, out, outLen, outCapacity);
        return;
    case Op::TRAP:
        if(instr.imm == 0)
        {
            putByte(103, out, outLen, outCapacity);
            return;
        }
        putByte(104, out, outLen, outCapacity);
        encodeLeb128((uint32_t)instr.imm, out, outLen, outCapacity);
        return;
    case Op::PUSH:
        putByte(105, out, outLen, outCapacity);
        return;
    case Op::LOAD:
        putByte(106, out, outLen, outCapacity);
        encodeLeb128(instr.target, out, outLen, outCapacity);
        return;
    case Op::STORE:
        putByte(107, out, outLen, outCapacity);
        encodeLeb128(instr.target, out, outLen, outCapacity);
        return;
    case Op::CONST:
        if(instr.imm >= 0 && instr.imm <= 15)
        {
            putByte((uint8_t)(109 + instr.imm), out, outLen, outCapacity);
            return;
        }
        putByte(108, out, outLen, outCapacity);
        encodeLeb128((uint32_t)instr.imm, out, outLen, outCapacity);
        return;
    default:
        break; // GCOV_EXCL_LINE — every Op not already handled by isArithOp/isComparisonOp/the unary range is one of the explicit cases above, with one deliberate exception: Op::EXT. An extension instruction's operands never live in Instr (they stay on the wire, isa-core.md §11.3), so this encoder structurally cannot rebuild one — a fixture needing an extension op splices its own bytes instead. Reaching here with anything else is a translator-logic bug.
    }

    assert(false && "encode_instr: unhandled instruction"); // GCOV_EXCL_LINE
}

uint32_t encodeBody(const Instr *body, uint32_t count, uint8_t *out, uint32_t outCapacity)
{
    uint32_t outLen = 0;
    for(uint32_t i = 0; i < count; i++)
    {
        encodeInstr(body[i], out, outLen, outCapacity);
    }
    return outLen;
}

uint32_t encodeProgram(const ProcSource *procs, uint32_t procCount, uint8_t *out, uint32_t outCapacity)
{
    uint32_t outLen = 0;
    encodeLeb128(procCount, out, outLen, outCapacity);
    for(uint32_t i = 0; i < procCount; i++)
    {
        encodeLeb128(procs[i].argCount, out, outLen, outCapacity);
        for(uint32_t j = 0; j < procs[i].bodyCount; j++)
        {
            encodeInstr(procs[i].body[j], out, outLen, outCapacity);
        }
    }
    return outLen;
}

uint32_t appendProgramFrame(uint8_t *out, uint32_t len, uint32_t outCapacity)
{
    assert(len + PROGRAM_FRAME_BYTES <= outCapacity); // GCOV_EXCL_LINE
    (void)outCapacity;

    BcReader wire;
    wire.open(bcMapped(out), len);

    const uint16_t frame = programFrameHash(wire, len);
    out[len] = (uint8_t)frame;
    out[len + 1] = (uint8_t)(frame >> 8);

    return len + PROGRAM_FRAME_BYTES;
}

FramedProgram framedProgram(const uint8_t *literal, uint32_t len)
{
    FramedProgram f{};
    assert(len + PROGRAM_FRAME_BYTES <= sizeof(f.bytes)); // GCOV_EXCL_LINE

    for(uint32_t i = 0; i < len; i++)
    {
        f.bytes[i] = literal[i];
    }
    f.len = appendProgramFrame(f.bytes, len, (uint32_t)sizeof(f.bytes));

    return f;
}

uint32_t encodeJitProgram(uint32_t maxCallDepth, uint32_t totalDepth, const ProcSource *procs, uint32_t procCount, uint8_t *out, uint32_t outCapacity)
{
    uint32_t outLen = 0;
    encodeLeb128(maxCallDepth, out, outLen, outCapacity);
    encodeLeb128(totalDepth, out, outLen, outCapacity);
    outLen += encodeProgram(procs, procCount, out + outLen, outCapacity - outLen);
    return appendProgramFrame(out, outLen, outCapacity);
}

} // namespace jitc
