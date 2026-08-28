// Diverse-shape Instr[] programs shared between fixtures.cpp (the real
// QEMU exercise) and test/tools/dump_corpus.cpp (a seed-corpus dump for
// whenever a fuzz harness gets built around decodeLeb128/translateProc/
// enterProgramSplit). Authored once here so both consumers see the exact
// same bytes rather than two independently-typed copies drifting apart.
// Host-portable (instr.h has no runtime/target dependency, same as every
// other fixtures.cpp program), inline so this header can be included from
// more than one translation unit without a separate .cpp.
#ifndef JIT_ARMV6M_TEST_CORPUS_PROGRAMS_H_
#define JIT_ARMV6M_TEST_CORPUS_PROGRAMS_H_

#include "instr.h"

namespace jitc
{

// Nested LOOP-in-LOOP, sum of triangular numbers — fixtures.cpp's fixture
// 28. See its own comment there for the full derivation.
inline constexpr Instr corpusNestedLoopProc0[] = {
    LOAD(0), PUSH(),
    CONST(0), PUSH(),
    CONST(0), PUSH(),
    CONST(0), PUSH(),
    bare(Op::LOOP),
        LOAD(1),
    bare(Op::BLOCK_END),
        LOAD(1), STORE(3),
        CONST(0), STORE(4),
        bare(Op::LOOP),
            LOAD(3),
        bare(Op::BLOCK_END),
            LOAD(4), opReg(Op::ADD, 3), STORE(4),
            LOAD(3), opImm(Op::SUB, 1), STORE(3),
        bare(Op::BLOCK_END),
        LOAD(2), opReg(Op::ADD, 4), STORE(2),
        LOAD(1), opImm(Op::SUB, 1), STORE(1),
    bare(Op::BLOCK_END),
    LOAD(2), bare(Op::RETURN),
};

// BR_TABLE nested inside a LOOP body — fixtures.cpp's fixture 29.
inline constexpr Instr corpusBrTableInLoopProc0[] = {
    LOAD(0), PUSH(),
    CONST(0), PUSH(),
    bare(Op::LOOP),
        LOAD(1),
    bare(Op::BLOCK_END),
        LOAD(1), opImm(Op::AND, 1), brTable(2),
            LOAD(1), opImm(Op::MUL, 10), opReg(Op::ADD, 2), STORE(2), bare(Op::BLOCK_END),
            LOAD(1), opReg(Op::ADD, 2), STORE(2), bare(Op::BLOCK_END),
        LOAD(1), opImm(Op::SUB, 1), STORE(1),
    bare(Op::BLOCK_END),
    LOAD(2), bare(Op::RETURN),
};

// LOOP nested inside a BR_TABLE case — fixtures.cpp's fixture 30.
inline constexpr Instr corpusLoopInBrTableProc0[] = {
    LOAD(0), opImm(Op::SHR, 8), brTable(2),
        CONST(0), PUSH(),
        LOAD(0), opImm(Op::AND, 0xFF), PUSH(),
        bare(Op::LOOP),
            LOAD(2),
        bare(Op::BLOCK_END),
            LOAD(1), opReg(Op::ADD, 2), STORE(1),
            LOAD(2), opImm(Op::SUB, 1), STORE(2),
        bare(Op::BLOCK_END),
        POP(),
        POP(),
        bare(Op::BLOCK_END),
        LOAD(0), opImm(Op::AND, 0xFF), opImm(Op::MUL, 3), bare(Op::BLOCK_END),
    bare(Op::RETURN),
};

// Large BR_TABLE (N=20) with a CALL inside one case — fixtures.cpp's
// fixture 31.
inline constexpr Instr corpusLargeBrTableProc0[] = {
    LOAD(0), brTable(20),
        CONST(0), bare(Op::BLOCK_END),
        CONST(10), bare(Op::BLOCK_END),
        CONST(20), bare(Op::BLOCK_END),
        CONST(30), bare(Op::BLOCK_END),
        CONST(40), bare(Op::BLOCK_END),
        CONST(50), bare(Op::BLOCK_END),
        CONST(60), bare(Op::BLOCK_END),
        CONST(5), call(1), bare(Op::BLOCK_END),
        CONST(80), bare(Op::BLOCK_END),
        CONST(90), bare(Op::BLOCK_END),
        CONST(100), bare(Op::BLOCK_END),
        CONST(110), bare(Op::BLOCK_END),
        CONST(120), bare(Op::BLOCK_END),
        CONST(130), bare(Op::BLOCK_END),
        CONST(140), bare(Op::BLOCK_END),
        CONST(150), bare(Op::BLOCK_END),
        CONST(160), bare(Op::BLOCK_END),
        CONST(170), bare(Op::BLOCK_END),
        CONST(180), bare(Op::BLOCK_END),
        CONST(190), bare(Op::BLOCK_END),
    bare(Op::RETURN),
};
inline constexpr Instr corpusLargeBrTableProc1[] = {LOAD(0), opImm(Op::ADD, 1000), bare(Op::RETURN)};

// Deep operand stack, 24 live locals — fixtures.cpp's fixture 32.
inline constexpr Instr corpusDeepStackProc0[] = {
    CONST(1), PUSH(), CONST(2), PUSH(), CONST(3), PUSH(), CONST(4), PUSH(),
    CONST(5), PUSH(), CONST(6), PUSH(), CONST(7), PUSH(), CONST(8), PUSH(),
    CONST(9), PUSH(), CONST(10), PUSH(), CONST(11), PUSH(), CONST(12), PUSH(),
    CONST(13), PUSH(), CONST(14), PUSH(), CONST(15), PUSH(), CONST(16), PUSH(),
    CONST(17), PUSH(), CONST(18), PUSH(), CONST(19), PUSH(), CONST(20), PUSH(),
    CONST(21), PUSH(), CONST(22), PUSH(), CONST(23), PUSH(), CONST(24), PUSH(),
    CONST(0),
    opReg(Op::ADD, 0), opReg(Op::ADD, 1), opReg(Op::ADD, 2), opReg(Op::ADD, 3),
    opReg(Op::ADD, 4), opReg(Op::ADD, 5), opReg(Op::ADD, 6), opReg(Op::ADD, 7),
    opReg(Op::ADD, 8), opReg(Op::ADD, 9), opReg(Op::ADD, 10), opReg(Op::ADD, 11),
    opReg(Op::ADD, 12), opReg(Op::ADD, 13), opReg(Op::ADD, 14), opReg(Op::ADD, 15),
    opReg(Op::ADD, 16), opReg(Op::ADD, 17), opReg(Op::ADD, 18), opReg(Op::ADD, 19),
    opReg(Op::ADD, 20), opReg(Op::ADD, 21), opReg(Op::ADD, 22), opReg(Op::ADD, 23),
    bare(Op::RETURN),
};

} // namespace jitc

#endif // JIT_ARMV6M_TEST_CORPUS_PROGRAMS_H_
