// jit-armv6m/compiler/test — decode_instr.h/encode_instr.h round-trip.
// One instruction of every shape decode_instr.h claims to handle, encoded
// via encode_instr.h and decoded back — cross-checks the two against each
// other using hand-derived cases, since there's no reference
// implementation here to check against directly.
#include "Test.h"
#include "decode_instr.h"
#include "wire.h"
#include "encode_instr.h"

#include <cstring>

using namespace jitc;

static bool sameInstr(const Instr &a, const Instr &b)
{
    if(a.op != b.op || a.combo != b.combo)
    {
        return false; // GCOV_EXCL_LINE — only reached by a failing round trip
    }
    if(a.op == Op::CALL)
    {
        return a.calleeIndex == b.calleeIndex;
    }
    if(a.combo == Combo::REG_ACC || a.combo == Combo::REG_REG || a.op == Op::LOAD || a.op == Op::STORE)
    {
        return a.target == b.target;
    }
    if(a.combo == Combo::IMM_ACC || a.op == Op::CONST || a.op == Op::TRAP || a.op == Op::BR_TABLE)
    {
        return a.imm == b.imm;
    }
    return true;
}

static const Instr kCases[] = {
    opReg(Op::ADD, 3), opRegWriteback(Op::SUB, 1), opStack(Op::MUL, Combo::PEEK_PEEK),
    opStack(Op::AND, Combo::POP_ACC), opImm(Op::XOR, 42),
    opReg(Op::EQ, 2), opStack(Op::LT_S, Combo::POP_ACC), opImm(Op::EQ, 0), opImm(Op::GE_U, 100),
    bare(Op::NEG), bare(Op::NOT), bare(Op::SXTB), bare(Op::UXTH),
    bare(Op::CLZ), bare(Op::REVBITS), // §5.3's MISC_UNARY escape
    bare(Op::BLOCK_END), bare(Op::LOOP), brTable(1), brTable(2), brTable(5),
    call(3), bare(Op::RETURN), trapInstr(0), trapInstr(7),
    PUSH(), LOAD(4), STORE(9), CONST(3), CONST(12345), CONST(-1),
};
static constexpr uint32_t kCaseCount = sizeof(kCases) / sizeof(kCases[0]);

TEST(DecodeInstrRoundTripsEveryShape)
{
    for(uint32_t i = 0; i < kCaseCount; i++)
    {
        uint8_t bytes[8];
        uint32_t len = 0;
        encodeInstr(kCases[i], bytes, len, sizeof(bytes));

        WireInstr d = decodeOne(bytes, len);
        CHECK(sameInstr(d.instr, kCases[i]));
        CHECK(d.consumed == len);
    }
}

TEST(DecodeInstrAdvancesThroughARunOfInstructions)
{
    const Instr program[] = {opImm(Op::ADD, 5), LOAD(0), bare(Op::RETURN)};
    uint8_t bytes[32];
    uint32_t len = encodeBody(program, 3, bytes, sizeof(bytes));

    BcReader r = wireOver(bytes, len);
    for(uint32_t i = 0; i < 3; i++)
    {
        CHECK(sameInstr(decodeFrom(r).instr, program[i]));
    }
    CHECK(r.atEnd());
}

TEST(EncodeConstSmallImmFitsOneByte)
{
    uint8_t bytes[8];
    uint32_t len = 0;
    encodeInstr(CONST(15), bytes, len, sizeof(bytes));
    CHECK(len == 1);
    CHECK(bytes[0] == 109 + 15);
}

TEST(EncodeConstLargeImmUsesLeb128Form)
{
    uint8_t bytes[8];
    uint32_t len = 0;
    encodeInstr(CONST(16), bytes, len, sizeof(bytes));
    CHECK(len == 2);
    CHECK(bytes[0] == 108);
    CHECK(bytes[1] == 16);
}

TEST(EncodeComparisonZeroImmCollapsesToOneByte)
{
    uint8_t bytes[8];
    uint32_t len = 0;
    encodeInstr(opImm(Op::EQ, 0), bytes, len, sizeof(bytes));
    CHECK(len == 1); // mode 2 ("IMM_ACC #0"), no operand byte
}

TEST(EncodeLeb128MultiByteRoundTrips)
{
    uint8_t bytes[8];
    uint32_t len = 0;
    encodeLeb128(300, bytes, len, sizeof(bytes)); // 300 = 0b1_0010_1100 -> 2 LEB128 bytes
    CHECK(len == 2);

    BcReader r = wireOver(bytes, len);
    uint32_t value = 0;
    CHECK(decodeLeb128(r, value));
    CHECK(value == 300);
    CHECK(r.atEnd());
}

// ── encodeProgram/encodeJitProgram (isa-core.md §5.5) ───────────────────

TEST(EncodeProgramNoHeaderTableEachArgCountSitsDirectlyBeforeItsOwnBody)
{
    // Mirrors bytecode.test.ts's own "no header table" test byte for
    // byte: proc_count(2), then each procedure's own arg_count
    // immediately followed by its own body — no separate header block,
    // no stored body length.
    const Instr body0[] = {bare(Op::RETURN)};                 // 1-byte body
    const Instr body1[] = {CONST(1), bare(Op::RETURN)};         // 2-byte body
    ProcSource procs[] = {
        {0, body0, 1},
        {0, body1, 2},
    };
    uint8_t bytes[16];
    uint32_t len = encodeProgram(procs, 2, bytes, sizeof(bytes));

    const uint8_t expected[] = {2, 0, 102, 0, 109 + 1, 102};
    CHECK(len == sizeof(expected));
    CHECK(memcmp(bytes, expected, sizeof(expected)) == 0);
}

TEST(EncodeProgramBodyDecodesBackToTheSameInstructions)
{
    const Instr body0[] = {CONST(5), call(1), bare(Op::RETURN)};
    const Instr body1[] = {LOAD(0), opImm(Op::ADD, 10), bare(Op::RETURN)};
    ProcSource procs[] = {
        {0, body0, 3},
        {1, body1, 3},
    };
    uint8_t bytes[32];
    uint32_t len = encodeProgram(procs, 2, bytes, sizeof(bytes));

    BcReader r = wireOver(bytes, len);
    uint32_t procCount = 0, argCount = 0;

    CHECK(decodeLeb128(r, procCount));
    CHECK(procCount == 2);

    CHECK(decodeLeb128(r, argCount));
    CHECK(argCount == 0);
    for(uint32_t i = 0; i < 3; i++)
    {
        CHECK(sameInstr(decodeFrom(r).instr, body0[i]));
    }

    CHECK(decodeLeb128(r, argCount));
    CHECK(argCount == 1);
    for(uint32_t i = 0; i < 3; i++)
    {
        CHECK(sameInstr(decodeFrom(r).instr, body1[i]));
    }
    CHECK(r.atEnd());
}

TEST(EncodeJitProgramPrependsTheStatsAndAppendsTheFrame)
{
    const Instr body0[] = {bare(Op::RETURN)};
    ProcSource procs[] = {{0, body0, 1}};

    uint8_t plain[16];
    uint32_t plainLen = encodeProgram(procs, 1, plain, sizeof(plain));

    uint8_t jit[16];
    uint32_t jitLen = encodeJitProgram(3, 300, procs, 1, jit, sizeof(jit));

    BcReader r = wireOver(jit, jitLen);
    uint32_t maxCallDepth = 0, totalDepth = 0;

    CHECK(decodeLeb128(r, maxCallDepth));
    CHECK(maxCallDepth == 3);
    CHECK(decodeLeb128(r, totalDepth));
    CHECK(totalDepth == 300); // multi-byte LEB128 — proves the chain, not just a single byte

    const uint32_t pos = jitLen - r.remaining();
    CHECK(jitLen - pos == plainLen + PROGRAM_FRAME_BYTES);
    CHECK(memcmp(jit + pos, plain, plainLen) == 0);

    const uint32_t payload = jitLen - PROGRAM_FRAME_BYTES;
    BcReader hash = wireOver(jit, payload);
    CHECK(programFrameHash(hash, payload) == (uint16_t)(jit[payload] | (jit[payload + 1] << 8)));

    BcReader whole = wireOver(jit, jitLen);
    CHECK(programFrameOk(whole, jitLen));
}

// ── The decode table against isa-core.md §5.2 itself ────────────────────

// §5.2's own orderings, transcribed straight from the spec rather than
// from decode_instr.cpp, so a typo in that file's table can't hide by
// agreeing with itself.
static const Op SPEC_ARITH[10] = {Op::ADD, Op::SUB, Op::RSUB, Op::MUL, Op::AND, Op::OR, Op::XOR, Op::SHL, Op::SHR, Op::ASR};
static const Combo SPEC_ARITH_MODE[5] = {Combo::REG_ACC, Combo::REG_REG, Combo::PEEK_PEEK, Combo::POP_ACC, Combo::IMM_ACC};
static const Op SPEC_CMP[10] = {Op::EQ, Op::NE, Op::LT_S, Op::LE_S, Op::GT_S, Op::GE_S, Op::LT_U, Op::LE_U, Op::GT_U, Op::GE_U};
static const Combo SPEC_CMP_MODE[4] = {Combo::REG_ACC, Combo::POP_ACC, Combo::IMM_ACC, Combo::IMM_ACC};
static const Op SPEC_MISC[19] = {
    Op::NEG, Op::NOT, Op::SXTB, Op::SXTH,                 // 90-93
    Op::UXTB, Op::UXTH,                                   // 94-95
    Op::BLOCK_END, Op::LOOP, Op::BR_TABLE,                // 96-98
    Op::FALLTHROUGH,                                      // 99
    Op::BR_TABLE, Op::CALL, Op::RETURN, Op::TRAP,         // 100-103
    Op::TRAP, Op::PUSH, Op::LOAD, Op::STORE,              // 104-107
    Op::CONST,                                            // 108
};
// §5.3's MISC_UNARY sub-codes, in sub-code order.
static const Op SPEC_MISC_UNARY[2] = {Op::REVBITS, Op::CLZ};
// Which assigned opcodes carry a trailing LEB128 (§5.4).
static bool specHasTrailingOperand(uint32_t code)
{
    if(code <= 49)
    {
        uint32_t mode = code % 5;
        return mode == 0 || mode == 1 || mode == 4;
    }
    if(code <= 89)
    {
        return (code - 50) % 4 == 0 || (code - 50) % 4 == 3;
    }
    return code == 100 || code == 101 || code == 104 || code == 106 || code == 107 || code == 108;
}

TEST(DecodeTableAgreesWithTheSpecsOwnFormulas)
{
    // decode_instr.cpp replaced §5.2's `code / 5` and `(code-50) / 4` with
    // a static table, because on a Cortex-M0 that division is a call into
    // libgcc. Here on the host division is free, so re-derive the spec's
    // formulas and check the table reproduces them for every assigned
    // opcode — op, combo, and whether an operand trails.
    for(uint32_t code = 0; code < MISC_BASE; code++)
    {
        // A 2-byte LEB128 tail, so a decoder that wrongly expects an
        // operand (or wrongly skips one) lands on the wrong `next`.
        const uint8_t bytes[] = {(uint8_t)code, 0x81, 0x01, 0x00};
        WireInstr d = decodeOne(bytes, sizeof(bytes));

        Op expectOp;
        Combo expectCombo = Combo::NONE;
        if(code <= 49)
        {
            expectOp = SPEC_ARITH[code / 5];
            expectCombo = SPEC_ARITH_MODE[code % 5];
        }
        else if(code <= 89)
        {
            expectOp = SPEC_CMP[(code - 50) / 4];
            expectCombo = SPEC_CMP_MODE[(code - 50) % 4];
        }
        else if(code <= 108)
        {
            expectOp = SPEC_MISC[code - 90];
        }
        else
        {
            expectOp = Op::CONST;
        }

        CHECK(d.instr.op == expectOp);
        CHECK(d.instr.combo == expectCombo);
        CHECK(d.consumed == (specHasTrailingOperand(code) ? 3u : 1u));
    }
}

TEST(DecodeSuppliesTheImplicitAuxValuesTheSpecPromises)
{
    // The forms whose auxiliary value comes from the opcode itself rather
    // than a trailing operand (§5.2's IMM_SMALL, BR_TABLE#1, TRAP#0,
    // CONST#0..15) — the ones a table has to carry as constants.
    const uint8_t brTable1[] = {98}, trap0[] = {103};
    CHECK(decodeOne(brTable1, 1).instr.imm == 1);
    CHECK(decodeOne(trap0, 1).instr.imm == 0);

    // §5.4's bias: the extended form's operand is N - 2, so it starts at 2
    // and neither 0 nor 1 has a second spelling.
    const uint8_t brTableExt0[] = {100, 0}, brTableExt1[] = {100, 1};
    CHECK(decodeOne(brTableExt0, 2).instr.imm == 2);
    CHECK(decodeOne(brTableExt1, 2).instr.imm == 3);

    for(uint32_t i = 0; i < 16; i++)
    {
        const uint8_t small[] = {(uint8_t)(109 + i)};
        WireInstr d = decodeOne(small, 1);
        CHECK(d.instr.op == Op::CONST);
        CHECK(d.instr.imm == (int32_t)i);
        CHECK(d.consumed == 1);
    }

    for(uint32_t cmp = 0; cmp < 10; cmp++)
    {
        const uint8_t immSmall[] = {(uint8_t)(50 + cmp * 4 + 2)};
        WireInstr d = decodeOne(immSmall, 1);
        CHECK(d.instr.combo == Combo::IMM_ACC);
        CHECK(d.instr.imm == 0);
    }
}

TEST(MiscUnaryEscapeDecodesItsAssignedSubCodes)
{
    // §5.3: the escape byte plus a sub-code, so two bytes for what used to
    // be one. Only MISC_UNARY has assigned sub-codes so far.
    for(uint32_t sub = 0; sub < 2; sub++)
    {
        const uint8_t bytes[] = {(uint8_t)MISC_UNARY, (uint8_t)sub, 0x00};
        WireInstr d = decodeOne(bytes, sizeof(bytes));
        CHECK(d.instr.op == SPEC_MISC_UNARY[sub]);
        CHECK(d.instr.combo == Combo::NONE);
        CHECK(d.consumed == 2);

        uint8_t reencoded[8];
        uint32_t len = 0;
        encodeInstr(d.instr, reencoded, len, sizeof(reencoded));
        CHECK(len == 2);
        CHECK(reencoded[0] == MISC_UNARY);
        CHECK(reencoded[1] == sub);
    }
}

TEST(OnlyMiscUnaryHasAssignedSubCodes)
{
    // MISC_CF and MISC_BINARY are both held empty (isa-core.md §5.3), and
    // MISC_UNARY stops at two.
    for(uint32_t code = MISC_BASE; code <= LAST_CORE_OPCODE; code++)
    {
        for(uint32_t sub = 0; sub < 4; sub++)
        {
            CHECK(miscSubCodeAssigned(code, sub) == (code == MISC_UNARY && sub < 2));
        }
    }
}

TEST(EveryAssignedOpcodeSurvivesAnEncodeDecodeCycle)
{
    // Byte-identity is the wrong invariant — the encoder canonicalizes
    // (TRAP#0 for a zero code, CONST#n for a small literal, IMM_SMALL for
    // a zero comparison operand), so a non-canonical input re-encodes to a
    // shorter form. What must hold is that the *instruction* is a fixed
    // point: decode, encode, decode again, and nothing has changed.
    for(uint32_t code = 0; code < MISC_BASE; code++)
    {
        const uint8_t bytes[] = {(uint8_t)code, 0x81, 0x01, 0x00};
        WireInstr first = decodeOne(bytes, sizeof(bytes));

        uint8_t reencoded[8];
        uint32_t len = 0;
        encodeInstr(first.instr, reencoded, len, sizeof(reencoded));
        CHECK(len > 0);

        WireInstr second = decodeOne(reencoded, len);
        CHECK(sameInstr(first.instr, second.instr));
        CHECK(second.consumed == len);
    }
}
