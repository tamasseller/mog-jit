// Multi-argument ENTRY procedures — the arguments enterDispatch marshals
// in, rather than the ones a compiled CALL passes.

#include <cstdint>

#include "instr.h"
#include "run_program.h"
#include "dispatch_abi.h"
#include "Test.h"

using namespace jitc;

// ---- Multi-argument ENTRY procedures.
//
// Every other TEST in this directory reaches an out-of-window argument through
// proc1 or deeper, never through proc0, because until enterDispatch learned to
// marshal an argument vector there was no way to give the entry procedure more
// than the single acc-borne word. Both halves of that gap are covered here: 2..4
// arguments (window registers enterDispatch never used to initialize, so they
// arrived holding the caller's r8-r11) and 5+ (where the epilogue also
// reclaimed a frame nobody had pushed, landing .Lresume on a shifted sp — a
// deterministic hang, not a wrong answer).
//
// Bodies pack their arguments into nibbles rather than summing them: a sum is
// invariant under any permutation of the window, which is precisely the error
// class most likely here. args {1,2,3,...} therefore expect 0x123..., and any
// swapped register or mis-ordered spill slot changes the result.

// Nibble-pack k = 0..N-1: acc = arg0, then (acc << 4) | arg_k.
#define PACK_ARG(k) opImm(Op::SHL, 4), opReg(Op::OR, k)

// Two arguments — entirely in-window. arg0 lands in physReg(0) = r7, which
// enterDispatch had no way to write at all.
static const Instr entryArgs2Proc0[] = {LOAD(0), PACK_ARG(1), bare(Op::RETURN)};

TEST(EntryWithTwoArguments)
{
    ProcSource procs[] = {PROC(2, entryArgs2Proc0)};
    uint32_t args[] = {1, 2};
    ProgramResult r = runProgram(procs, 1, args);

    CHECK(!r.trapped);
    CHECK(r.value == 0x12u);
}

// Four arguments — exactly fills the window, and exactly fillCalleeArgs's
// WINDOW_SIZE-1 cap (r7/r6/r5 supplied by the caller, r4 from acc). Still
// nothing spilled.
static const Instr entryArgs4Proc0[] = {LOAD(0), PACK_ARG(1), PACK_ARG(2), PACK_ARG(3), bare(Op::RETURN)};

TEST(EntryWithFourArguments)
{
    ProcSource procs[] = {PROC(4, entryArgs4Proc0)};
    uint32_t args[] = {1, 2, 3, 4};
    ProgramResult r = runProgram(procs, 1, args);

    CHECK(!r.trapped);
    CHECK(r.value == 0x1234u);
}

// Five arguments — one spilled word, read through spillOffset(0) == 0. Leaf,
// so the whole frame comes back via discardWindow's single ADD sp.
static const Instr entryArgs5Proc0[] = {
    LOAD(0), PACK_ARG(1), PACK_ARG(2), PACK_ARG(3), PACK_ARG(4), bare(Op::RETURN),
};

TEST(EntryWithFiveArguments)
{
    ProcSource procs[] = {PROC(5, entryArgs5Proc0)};
    uint32_t args[] = {1, 2, 3, 4, 5};
    ProgramResult r = runProgram(procs, 1, args);

    CHECK(!r.trapped);
    CHECK(r.value == 0x12345u);
}

// Six arguments — TWO spilled words, the smallest shape in which their order
// is observable at all (five spills exactly one, so a reversed push loop looks
// identical there).
static const Instr entryArgs6Proc0[] = {
    LOAD(0), PACK_ARG(1), PACK_ARG(2), PACK_ARG(3), PACK_ARG(4), PACK_ARG(5), bare(Op::RETURN),
};

TEST(EntryWithSixArguments)
{
    ProcSource procs[] = {PROC(6, entryArgs6Proc0)};
    uint32_t args[] = {1, 2, 3, 4, 5, 6};
    ProgramResult r = runProgram(procs, 1, args);

    CHECK(!r.trapped);
    CHECK(r.value == 0x123456u);
}

// Eight arguments — the post-wrap window phase (slots 4..6 in r7/r6/r5, slot
// 7 from acc into physReg(7) = r4) with four spilled words.
static const Instr entryArgs8Proc0[] = {
    LOAD(0), PACK_ARG(1), PACK_ARG(2), PACK_ARG(3),
    PACK_ARG(4), PACK_ARG(5), PACK_ARG(6), PACK_ARG(7), bare(Op::RETURN),
};

TEST(EntryWithEightArguments)
{
    ProcSource procs[] = {PROC(8, entryArgs8Proc0)};
    uint32_t args[] = {1, 2, 3, 4, 5, 6, 7, 8};
    ProgramResult r = runProgram(procs, 1, args);

    CHECK(!r.trapped);
    CHECK(r.value == 0x12345678u);
}

// Six arguments AND a nested CALL, so the entry procedure is savesLR. Its own
// push{lr} lands above the words enterDispatch placed, which is exactly the +4
// shift Window::spillOffset applies to k < initialSpilledCount — and the
// return goes through returnHelperFromStackReclaim with r2 = 8, on an *entry*
// frame. This is the shape that hangs deterministically without the fix;
// SavesLrReturnWithOutOfWindowArgs/TwoLevelSavesLrChain only ever reach it via
// proc1.
static const Instr entryArgs6CallProc0[] = {
    LOAD(0), PACK_ARG(1), PACK_ARG(2), PACK_ARG(3), PACK_ARG(4), PACK_ARG(5),
    PUSH(),           // keep the packed value while the callee runs
    CONST(0),
    call(1),
    opReg(Op::ADD, 6), // += the packed value
    bare(Op::RETURN),
};
static const Instr entryArgs6CallProc1[] = {CONST(1000), bare(Op::RETURN)};

TEST(EntryWithSixArgumentsAndANestedCall)
{
    ProcSource procs[] = {PROC(6, entryArgs6CallProc0), PROC(0, entryArgs6CallProc1)};
    uint32_t args[] = {1, 2, 3, 4, 5, 6};
    ProgramResult r = runProgram(procs, 2, args);

    CHECK(!r.trapped);
    CHECK(r.value == 0x123456u + 1000u);
}

// Six arguments, body TRAPs. trapHelper restores sp from runtime->savedSp,
// which is only correct if savedSp was captured *before* enterDispatch pushed
// the arguments. Captured after, .Lresume would pop those words as the
// caller's r8-r11 and return to a garbage pc — a hang, so this reports a wrong
// outcome rather than a wrong value.
static const Instr entryArgs6TrapProc0[] = {
    LOAD(0), PACK_ARG(1), PACK_ARG(2), PACK_ARG(3), PACK_ARG(4), PACK_ARG(5),
    trapInstr(0x123456),
};

TEST(EntryWithSixArgumentsThatTraps)
{
    ProcSource procs[] = {PROC(6, entryArgs6TrapProc0)};
    uint32_t args[] = {1, 2, 3, 4, 5, 6};
    ProgramResult r = runProgram(procs, 1, args);

    CHECK(r.trapped == LANDING_TRAP);
    CHECK(r.value == 0x123456u);
}

// Six-argument entry whose callee traps — the same unwind one level down,
// with the entry procedure's own out-of-window arguments live below the frame
// being discarded.
static const Instr entryArgs6CalleeTrapProc0[] = {
    LOAD(0), PACK_ARG(1), PACK_ARG(2), PACK_ARG(3), PACK_ARG(4), PACK_ARG(5),
    call(1), bare(Op::RETURN),
};
static const Instr entryArgs6CalleeTrapProc1[] = {trapInstr(4242)};

TEST(EntryWithSixArgumentsWhoseCalleeTraps)
{
    ProcSource procs[] = {PROC(6, entryArgs6CalleeTrapProc0), PROC(0, entryArgs6CalleeTrapProc1)};
    uint32_t args[] = {1, 2, 3, 4, 5, 6};
    ProgramResult r = runProgram(procs, 2, args);

    CHECK(r.trapped == LANDING_TRAP);
    CHECK(r.value == 4242);
}

#undef PACK_ARG
