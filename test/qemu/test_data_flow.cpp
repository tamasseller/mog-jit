// Data flow within one procedure: the register window, spilling past it,
// and the accumulator folds and aliasing hazards around both.

#include <cstdint>

#include "instr.h"
#include "corpus_programs.h"
#include "run_program.h"
#include "Test.h"

using namespace jitc;

// ---- Out-of-window LOAD/STORE/REG_ACC/REG_REG, no CALL.
static const Instr outOfWindowProc0[] = {
    CONST(10), PUSH(), // k=0
    CONST(20), PUSH(), // k=1
    CONST(30), PUSH(), // k=2
    CONST(40), PUSH(), // k=3
    CONST(50), PUSH(), // k=4 -- evicts k=0's register
    CONST(60), PUSH(), // k=5 -- evicts k=1's register; k=0,1 now spilled
    LOAD(0),                    // acc = 10 (out-of-window LOAD)
    opReg(Op::ADD, 1),           // acc = 10+20=30 (out-of-window REG_ACC operand)
    STORE(0),                     // k0 := 30 (out-of-window STORE)
    CONST(5),
    opRegWriteback(Op::ADD, 1),    // k1 := 5+20=25 (out-of-window REG_REG)
    LOAD(0),                        // acc = k0 = 30
    opReg(Op::ADD, 1),                // acc = 30 + k1(25) = 55
    bare(Op::RETURN),
};

TEST(OutOfWindowLoadStoreAndRegisterOperands)
{
    ProcSource procs[] = {PROC(0, outOfWindowProc0)};
    ProgramResult r = runProgram(procs, 1, nullptr);

    CHECK(!r.trapped);
    CHECK(r.value == 55);
}

// ---- Operand-fold.
static const Instr operandFoldProc0[] = {
    CONST(10), PUSH(), // a -- k=0
    CONST(20), PUSH(), // b -- k=1
    CONST(30), PUSH(), // c -- k=2
    CONST(40), PUSH(), // d -- k=3
    LOAD(0),             // acc = a; accState depends on physReg(0)
    PUSH(),                // e = a -- k=4, evicts k=0's register
    bare(Op::RETURN),
};

TEST(RotationOperandFold)
{
    ProcSource procs[] = {PROC(0, operandFoldProc0)};
    ProgramResult r = runProgram(procs, 1, nullptr);

    CHECK(!r.trapped);
    CHECK(r.value == 10);
}

// ---- Destination-fold.
static const Instr destinationFoldProc0[] = {
    CONST(10), PUSH(),
    CONST(20), PUSH(),
    CONST(30), PUSH(),
    CONST(40), PUSH(),
    CONST(99), STORE(0), // a := 99; accState depends on physReg(0)
    PUSH(),                // e = a (now 99) -- k=4, evicts k=0's register
    bare(Op::RETURN),
};

TEST(RotationDestinationFold)
{
    ProcSource procs[] = {PROC(0, destinationFoldProc0)};
    ProgramResult r = runProgram(procs, 1, nullptr);

    CHECK(!r.trapped);
    CHECK(r.value == 99);
}

// ---- A comparison feeds further arithmetic — (n > 4) * 5.
static const Instr comparisonProc0[] = {LOAD(0), opImm(Op::GT_U, 4), opImm(Op::MUL, 5), bare(Op::RETURN)};

TEST(ComparisonFeedingArithmeticWhenTrue)
{
    ProcSource procs[] = {PROC(1, comparisonProc0)};
    uint32_t args[] = {6};
    ProgramResult r = runProgram(procs, 1, args);

    CHECK(!r.trapped);
    CHECK(r.value == 5);
}

TEST(ComparisonFeedingArithmeticWhenFalse)
{
    ProcSource procs[] = {PROC(1, comparisonProc0)};
    uint32_t args[] = {3};
    ProgramResult r = runProgram(procs, 1, args);

    CHECK(!r.trapped);
    CHECK(r.value == 0);
}

// ---- PEEK_PEEK two-op-in-place. 10 & 12 = 8.
static const Instr peekPeekProc0[] = {
    CONST(12), PUSH(),                 // k=0 = 12
    CONST(10),                          // acc = 10 (pending)
    opStack(Op::AND, Combo::PEEK_PEEK), // k0 := 10 & 12 = 8; acc poisoned
    LOAD(0), bare(Op::RETURN),
};

TEST(PeekPeekTwoOperandInPlace)
{
    ProcSource procs[] = {PROC(0, peekPeekProc0)};
    ProgramResult r = runProgram(procs, 1, nullptr);

    CHECK(!r.trapped);
    CHECK(r.value == 8);
}

// ---- Regression for emitAddSubRsub with accShape and the IMM_ACC operand
// both compile-time immediates (CONST directly followed by an immediate
// arithmetic op), with the combined immediate too large for the imm3/imm8
// fast paths — materializing accShape into SCRATCH_REG and then the second
// immediate into that same register clobbers the first value, silently
// computing `k op k` instead of `accShape op k`.
static const Instr bothImmediateProc0[] = {CONST(5), opImm(Op::ADD, 1000), bare(Op::RETURN)};

TEST(BothImmediateOperandAliasing)
{
    ProcSource procs[] = {PROC(0, bothImmediateProc0)};
    ProgramResult r = runProgram(procs, 1, nullptr);

    CHECK(!r.trapped);
    CHECK(r.value == 1005);
}

// ---- The same aliasing bug class, but the operand register itself happens
// to be SCRATCH_REG, which happens for real whenever an out-of-window local
// gets reloaded via ldrSp(SCRATCH_REG, ...). argCount=1 starts with slot 0
// (the argument) in the window (tos=1); four PUSHes bring tos to 5, exactly
// the point (inWindow's own `tos - k <= WINDOW_SIZE`) where slot 0 gets
// evicted onto the real stack, so `opReg(ADD, 0)` must reload it through
// SCRATCH_REG right after CONST(100) leaves acc pending.
static const Instr scratchOperandProc0[] = {
    CONST(2), PUSH(), // slot1=2, tos=2
    CONST(3), PUSH(), // slot2=3, tos=3
    CONST(4), PUSH(), // slot3=4, tos=4
    CONST(5), PUSH(), // slot4=5, tos=5 — slot 0 (the argument) now spilled
    CONST(100),
    opReg(Op::ADD, 0),
    bare(Op::RETURN),
};

TEST(ScratchRegOperandAliasingWithOne)
{
    ProcSource procs[] = {PROC(1, scratchOperandProc0)};
    uint32_t args[] = {1};
    ProgramResult r = runProgram(procs, 1, args);

    CHECK(!r.trapped);
    CHECK(r.value == 101);
}

TEST(ScratchRegOperandAliasingWithFortyTwo)
{
    ProcSource procs[] = {PROC(1, scratchOperandProc0)};
    uint32_t args[] = {42};
    ProgramResult r = runProgram(procs, 1, args);

    CHECK(!r.trapped);
    CHECK(r.value == 142);
}

// ---- Deep operand stack, 24 live locals. Everything past k3 is
// out-of-window (WINDOW_SIZE=4), so the chain of ADDs reload-addresses 20
// spilled slots in one procedure — several times deeper than
// MoreStackArgumentsThanTheWindowHolds' 6 stack args. Body lives in
// corpus_programs.h.
TEST(DeepOperandStack)
{
    ProcSource procs[] = {PROC(0, corpusDeepStackProc0)};
    ProgramResult r = runProgram(procs, 1, nullptr);

    CHECK(!r.trapped);
    CHECK(r.value == 300); // 1+2+...+24
}

// ---- Acc-fold thrash inside a loop. Per iteration:
// total := (total + 7) & 0xF -- an operand-fold-eligible ADD immediately
// followed by a never-folds AND, repeated across several loop iterations
// (RotationOperandFold/RotationDestinationFold/PeekPeekTwoOperandInPlace
// exercise fold/no-fold transitions exactly once; this repeats it every
// back-edge).
static const Instr accFoldThrashProc0[] = {
    LOAD(0), PUSH(),  // k1 = counter := n
    CONST(0), PUSH(), // k2 = total := 0
    bare(Op::LOOP_PRE),
        LOAD(2), opImm(Op::ADD, 7), opImm(Op::AND, 0xF), STORE(2), // total := (total+7)&0xF
        LOAD(1), opImm(Op::SUB, 1), STORE(1),
    bare(Op::BLOCK_END),
        LOAD(1),
    bare(Op::BLOCK_END), // back-edge while counter != 0
    LOAD(2), bare(Op::RETURN),
};

TEST(AccFoldThrashInsideALoop)
{
    ProcSource procs[] = {PROC(1, accFoldThrashProc0)};
    uint32_t args[] = {5};
    ProgramResult r = runProgram(procs, 1, args);

    CHECK(!r.trapped);
    CHECK(r.value == 3); // 0->7->14->5->12->3
}
