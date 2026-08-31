// The four extend ops (isa-core.md §4.3), on real translated code. Each is
// one ARMv6-M instruction, so what is worth checking here is the *value*
// crossing the acc/window boundary the same way NEG and NOT do — including
// the STORE fold, which is the only path where the result lands somewhere
// other than acc.

#include <cstdint>

#include "instr.h"
#include "run_program.h"
#include "Test.h"

using namespace jitc;

namespace
{

// acc = arg0, extended. One instruction between LOAD and RETURN.
const Instr sxtbProc0[] = {LOAD(0), bare(Op::SXTB), bare(Op::RETURN)};
const Instr sxthProc0[] = {LOAD(0), bare(Op::SXTH), bare(Op::RETURN)};
const Instr uxtbProc0[] = {LOAD(0), bare(Op::UXTB), bare(Op::RETURN)};
const Instr uxthProc0[] = {LOAD(0), bare(Op::UXTH), bare(Op::RETURN)};

uint32_t extend(const Instr *body, uint32_t count, uint32_t value)
{
    ProcSource procs[] = {ProcSource{1, body, count}};
    uint32_t args[] = {value};
    ProgramResult r = runProgram(procs, 1, args);
    CHECK(!r.trapped);
    return r.value;
}

#define EXTEND(body, value) extend(body, sizeof(body) / sizeof(body[0]), value)

} // namespace

TEST(SxtbSignExtendsTheLowByte)
{
    CHECK(EXTEND(sxtbProc0, 0x7f) == 0x7f);
    CHECK(EXTEND(sxtbProc0, 0x80) == 0xffffff80);
    CHECK(EXTEND(sxtbProc0, 0xdeadbeef) == 0xffffffef);
}

TEST(SxthSignExtendsTheLowHalfword)
{
    CHECK(EXTEND(sxthProc0, 0x7fff) == 0x7fff);
    CHECK(EXTEND(sxthProc0, 0x8000) == 0xffff8000);
    CHECK(EXTEND(sxthProc0, 0xdeadbeef) == 0xffffbeef);
}

TEST(UxtbAndUxthZeroExtendInstead)
{
    CHECK(EXTEND(uxtbProc0, 0xdeadbeef) == 0xef);
    CHECK(EXTEND(uxthProc0, 0xdeadbeef) == 0xbeef);
    CHECK(EXTEND(uxtbProc0, 0xffffffff) == 0xff);
    CHECK(EXTEND(uxthProc0, 0xffffffff) == 0xffff);
}

// The peekStoreFold path: an extend followed by STORE writes straight into
// the slot's register, so the value never passes through acc at all. Reads
// the slot back afterwards, which is the only way to see where it landed.
static const Instr foldedProc0[] = {
    CONST(0), PUSH(),            // k=0, the slot the fold writes into
    LOAD(1), bare(Op::SXTB), STORE(0),
    LOAD(0), bare(Op::RETURN),
};

TEST(AnExtendFoldsIntoTheStoreThatFollowsIt)
{
    ProcSource procs[] = {PROC(2, foldedProc0)};
    uint32_t args[] = {0, 0x1ff};
    ProgramResult r = runProgram(procs, 1, args);

    CHECK(!r.trapped);
    CHECK(r.value == 0xffffffff); // 0xff sign-extended, not 0x1ff
}

// Out of the four-register window, so the operand is a spill slot rather
// than a register — the extend still applies to acc, and the result still
// has to reach the caller.
static const Instr spilledProc0[] = {
    CONST(1), PUSH(), CONST(2), PUSH(), CONST(3), PUSH(),
    CONST(4), PUSH(), CONST(5), PUSH(),
    LOAD(0), bare(Op::UXTB), bare(Op::RETURN),
};

TEST(AnExtendWorksWithTheWindowSpilled)
{
    ProcSource procs[] = {PROC(1, spilledProc0)};
    uint32_t args[] = {0xcafebabe};
    ProgramResult r = runProgram(procs, 1, args);

    CHECK(!r.trapped);
    CHECK(r.value == 0xbe);
}
