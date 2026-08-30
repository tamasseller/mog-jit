// Unary ops, on an in-window operand and on a spilled one.

#include <cstdint>

#include "instr.h"
#include "run_program.h"
#include "Test.h"

using namespace jitc;

// ---- Unary ops, one procedure per op.
static const Instr negProc0[] = {LOAD(0), bare(Op::NEG), bare(Op::RETURN)};
static const Instr notProc0[] = {LOAD(0), bare(Op::NOT), bare(Op::RETURN)};
static const Instr clzProc0[] = {LOAD(0), bare(Op::CLZ), bare(Op::RETURN)};
static const Instr revbitsProc0[] = {LOAD(0), bare(Op::REVBITS), bare(Op::RETURN)};

TEST(UnaryNeg)
{
    ProcSource procs[] = {PROC(1, negProc0)};
    uint32_t args[] = {5};
    ProgramResult r = runProgram(procs, 1, args);

    CHECK(!r.trapped);
    CHECK(r.value == 0xFFFFFFFBu);
}

TEST(UnaryNot)
{
    ProcSource procs[] = {PROC(1, notProc0)};
    uint32_t args[] = {5};
    ProgramResult r = runProgram(procs, 1, args);

    CHECK(!r.trapped);
    CHECK(r.value == 0xFFFFFFFAu);
}

TEST(UnaryClzOfOne)
{
    ProcSource procs[] = {PROC(1, clzProc0)};
    uint32_t args[] = {1};
    ProgramResult r = runProgram(procs, 1, args);

    CHECK(!r.trapped);
    CHECK(r.value == 31);
}

TEST(UnaryClzOfZero)
{
    ProcSource procs[] = {PROC(1, clzProc0)};
    uint32_t args[] = {0};
    ProgramResult r = runProgram(procs, 1, args);

    CHECK(!r.trapped);
    CHECK(r.value == 32);
}

TEST(UnaryRevbits)
{
    ProcSource procs[] = {PROC(1, revbitsProc0)};
    uint32_t args[] = {1};
    ProgramResult r = runProgram(procs, 1, args);

    CHECK(!r.trapped);
    CHECK(r.value == 0x80000000u);
}

// ---- NEG/NOT consuming an out-of-window (spilled) operand. Mirrors
// UnaryNeg/UnaryNot, but with 4 PUSHes ahead of the arg so k0 is spilled by
// the time LOAD(0) reloads it, immediately followed by the unary op -- the
// exact shape emitUnary's src-parameter path needs (reading the reload's own
// destination register directly instead of forcing a flush through ACC_REG
// first). No other test here covers a spilled operand for a unary op.
static const Instr spilledNegProc0[] = {
    CONST(1), PUSH(), CONST(2), PUSH(), CONST(3), PUSH(), CONST(4), PUSH(),
    LOAD(0), bare(Op::NEG), bare(Op::RETURN),
};
static const Instr spilledNotProc0[] = {
    CONST(1), PUSH(), CONST(2), PUSH(), CONST(3), PUSH(), CONST(4), PUSH(),
    LOAD(0), bare(Op::NOT), bare(Op::RETURN),
};

TEST(UnaryNegOnASpilledOperand)
{
    ProcSource procs[] = {PROC(1, spilledNegProc0)};
    uint32_t args[] = {5};
    ProgramResult r = runProgram(procs, 1, args);

    CHECK(!r.trapped);
    CHECK(r.value == 0xFFFFFFFBu);
}

TEST(UnaryNotOnASpilledOperand)
{
    ProcSource procs[] = {PROC(1, spilledNotProc0)};
    uint32_t args[] = {5};
    ProgramResult r = runProgram(procs, 1, args);

    CHECK(!r.trapped);
    CHECK(r.value == 0xFFFFFFFAu);
}
