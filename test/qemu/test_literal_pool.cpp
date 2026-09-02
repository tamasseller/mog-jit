// The literal pool: how pooled words are reached, flushed, and stayed aligned to.

#include <cstdint>

#include "instr.h"
#include "run_program.h"
#include "Test.h"

using namespace jitc;

// ---- Literal pooling, both routes at once. CONST's own hard-to-synthesize
// value pools, and so does the ADD's immediate operand (which reaches the
// pool through Combo::IMM_ACC rather than CONST), so this executes two
// PC-relative loads at different alignment parities.
static const Instr pooledLiteralProc0[] = {CONST(0x12345678), opImm(Op::ADD, 0x11111111), bare(Op::RETURN)};

TEST(PooledLiteralViaBothRoutes)
{
    ProcSource procs[] = {PROC(0, pooledLiteralProc0)};
    ProgramResult r = runProgram(procs, 1, nullptr);

    CHECK(!r.trapped);
    CHECK(r.value == 0x23456789u);
}

// ---- A pooled load whose pool is flushed mid-procedure, with the flush's
// branch-around actually executed. A jump-table BR_TABLE forces the flush
// (its jump table's raw halfwords must not land in a later scan window), so
// the pool lands in the middle of the code and control has to jump over it
// to reach the dispatch. The pooled value is read back afterwards from a
// local, proving both the load and the jump-around worked.
static const Instr pooledFlushProc0[] = {
    CONST(0xDEADBEEF), PUSH(), // pooled — the chunk is open across the BR_TABLE
    CONST(1), brTable(2),       // forces the flush, mid-code, before the table
        CONST(100), bare(Op::BLOCK_END),
        CONST(200), bare(Op::BLOCK_END),
        CONST(300), bare(Op::BLOCK_END),
    LOAD(0),
    bare(Op::RETURN),
};

TEST(PooledLiteralFlushedMidProcedure)
{
    ProcSource procs[] = {PROC(0, pooledFlushProc0)};
    ProgramResult r = runProgram(procs, 1, nullptr);

    CHECK(!r.trapped);
    CHECK(r.value == 0xDEADBEEFu);
}

// ---- A pooled load in a procedure that does *not* start at the arena base.
// proc0 compiles to an odd number of halfwords (38 bytes, i.e. 2 mod 4), so
// without Runtime::allocate rounding its reservation up proc1 would land 2
// bytes off a word boundary — and every Align(pc,4)-based literal offset in
// it, resolved procedure-relative at translation time, would then read 2
// bytes away from its own pool word. This is the test that actually fails if
// that rounding is dropped.
static const Instr unalignedPoolProc0[] = {CONST(5), call(1), opImm(Op::ADD, 1), bare(Op::RETURN)};
static const Instr unalignedPoolProc1[] = {LOAD(0), opImm(Op::XOR, 0x0F0F0F0F), bare(Op::RETURN)};

TEST(PooledLiteralPastAnOddSizedProcedure)
{
    ProcSource procs[] = {PROC(0, unalignedPoolProc0), PROC(1, unalignedPoolProc1)};
    ProgramResult r = runProgram(procs, 2, nullptr);

    CHECK(!r.trapped);
    CHECK(r.value == 0x0F0F0F0Bu); // 5 ^ 0x0F0F0F0F = 0x0F0F0F0A, + 1
}
