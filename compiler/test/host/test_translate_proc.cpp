// Compiles a two-procedure program ("single-argument call, entirely
// acc-passed") end to end and checks the entire emitted halfword array
// against literals hand-derived from the ARMv6-M encoding tables and
// cross-checked against arm-none-eabi-as — proves the whole pipeline
// (bytecode -> Emitter/Window/AccState/binops/abi_strategy) composes
// correctly without QEMU.
#include "Test.h"
#include "translate_proc.h"
#include "encode_instr.h"

using namespace jitc;

namespace
{
// proc0 (argCount 0): CONST(37), call(1), RETURN
const Instr kProc0Body[] = {CONST(37), call(1), bare(Op::RETURN)};
// proc1 (argCount 1): LOAD(0), opImm(ADD, 5), RETURN
const Instr kProc1Body[] = {LOAD(0), opImm(Op::ADD, 5), bare(Op::RETURN)};
const uint32_t kArgCounts[] = {0, 1};

/** Encodes an Instr[] fixture into the raw wire bytes Proc::body expects. */
Proc makeProc(uint32_t argCount, const Instr *body, uint32_t count, uint8_t *bytesOut, uint32_t bytesCap)
{
    uint32_t len = encodeBody(body, count, bytesOut, bytesCap);
    return Proc{argCount, bytesOut, len};
}
}

TEST(TranslateProc0EntryProcedure)
{
    // proc0 makes a CALL, so it's non-leaf (savesLR): the prologue gains
    // push{lr}, and RETURN dispatches through returnHelperFromStack
    // (index 2, offset 8). argCount=0 keeps initialSpilledCount at 0 —
    // the ordinary non-leaf case, not the inline-pop-and-reclaim one.
    uint16_t buf[32];
    uint8_t bodyBytes[16];
    Proc proc = makeProc(0, kProc0Body, 3, bodyBytes, sizeof(bodyBytes));
    TranslateResult r = translateProc(proc, /*procIdx=*/0, kArgCounts, 2, buf, 32);

    CHECK(!r.overflowed);
    CHECK(r.halfwordCount == 18);

    const uint16_t expected[] = {
        0x465B, 0x604B, 0x3301, 0x469B, 0x447A, 0x4710, // prologue stub
        0xB500,                                          // PUSH {lr}  (savesLR — proc0 makes a CALL)
        0x2025,                                          // MOVS r0, #37  (CONST 37, stays pending until CALL flushes it)
        0x2113, 0x0209, 0x0209,                           // record synth: MOVS r1,#0x13; LSLS r1,r1,#8 (x2)
        0x2201,                                            // MOVS r2, #1  (calleeIndex=1, fits imm8)
        0x4653, 0x681B, 0x4718,                            // MOV r3,r10; LDR r3,[r3,#0]; BX r3  (callHelper)
        0x4653, 0x689B, 0x4718,                            // MOV r3,r10; LDR r3,[r3,#8]; BX r3  (returnHelperFromStack, index 2)
    };
    for(uint32_t i = 0; i < r.halfwordCount; i++)
    {
        CHECK(buf[i] == expected[i]);
    }
}

TEST(TranslateProc1Callee)
{
    // Last-argument fold: proc1's only reference to slot 0 (its last
    // argument) is body[0]'s own LOAD, so the callee prologue elides
    // both the unconditional flush into physReg(0) and that LOAD — the
    // argument stays PENDING in ACC_REG instead of round-tripping
    // through r7.
    uint16_t buf[32];
    uint8_t bodyBytes[16];
    Proc proc = makeProc(1, kProc1Body, 3, bodyBytes, sizeof(bodyBytes));
    TranslateResult r = translateProc(proc, /*procIdx=*/1, kArgCounts, 2, buf, 32);

    CHECK(!r.overflowed);
    CHECK(r.halfwordCount == 10);

    const uint16_t expected[] = {
        0x465B, 0x604B, 0x3301, 0x469B, 0x447A, 0x4710, // prologue stub
        0x1D40,                                            // ADDS r0, r0, #5  (LOAD(0)+opImm(ADD,5): LOAD elided, folded straight into acc)
        0x4653, 0x685B, 0x4718,                             // returnHelper tail
    };
    for(uint32_t i = 0; i < r.halfwordCount; i++)
    {
        CHECK(buf[i] == expected[i]);
    }
}

TEST(OverflowIsReportedRatherThanOverrunningTheBuffer)
{
    uint16_t buf[4]; // too small for even the 6-halfword prologue alone
    uint8_t bodyBytes[16];
    Proc proc = makeProc(0, kProc0Body, 3, bodyBytes, sizeof(bodyBytes));
    TranslateResult r = translateProc(proc, 0, kArgCounts, 2, buf, 4);
    CHECK(r.overflowed);
}

// The tests below exercise LOOP/BR_TABLE/comparisons-as-values/unary ops/
// last-argument-fold/block-nesting-overflow through translateProc()'s
// main loop. End-to-end behavioral correctness for all of these is
// already proven on real QEMU (test/qemu/fixtures.cpp), but that binary
// isn't gcov-instrumented, so it doesn't contribute to this host suite's
// own coverage. Sizes below are the measured halfwordCount for each body
// — a structural regression guard, not a full hex dump (that's the QEMU
// fixtures' job).

TEST(LoopClosesNormallyViaBlockEndBackEdge)
{
    // A real back-edge close (not terminator-closed) — exercises
    // closeBlockEnd's own LoopCond->LoopBody transition and the
    // unconditional back-edge branch it emits.
    const Instr body[] = {
        CONST(3), PUSH(),
        bare(Op::LOOP),
            LOAD(0),
        bare(Op::BLOCK_END),
            LOAD(0), opImm(Op::SUB, 1), STORE(0),
        bare(Op::BLOCK_END),
        bare(Op::RETURN),
    };
    uint32_t argCounts[] = {0};
    uint8_t bodyBytes[32];
    Proc proc = makeProc(0, body, sizeof(body) / sizeof(body[0]), bodyBytes, sizeof(bodyBytes));
    uint16_t buf[32];
    TranslateResult r = translateProc(proc, 0, argCounts, 1, buf, 32);
    CHECK(!r.overflowed);
    CHECK(r.halfwordCount == 17);
}

TEST(BrTableJumpTableHelperViaFullPipeline)
{
    // N > 2 reaches openBrTableJump's own MOV/LDR/BLX-through-helper-
    // vector call sequence into brTableJumpHelper (jit-armv6m/runtime/
    // runtime.S) — not reachable through the N<=2 fusion path this
    // file's other tests exercise.
    const Instr body[] = {
        CONST(1), brTable(3),
            CONST(10), bare(Op::BLOCK_END),
            CONST(20), bare(Op::BLOCK_END),
            CONST(30), bare(Op::BLOCK_END),
        bare(Op::RETURN),
    };
    uint32_t argCounts[] = {0};
    uint8_t bodyBytes[32];
    Proc proc = makeProc(0, body, sizeof(body) / sizeof(body[0]), bodyBytes, sizeof(bodyBytes));
    uint16_t buf[64];
    TranslateResult r = translateProc(proc, 0, argCounts, 1, buf, 64);
    CHECK(!r.overflowed);
    CHECK(r.halfwordCount == 24);
}

TEST(ComparisonFusesIntoBrTableGuard)
{
    const Instr body[] = {
        CONST(5), opImm(Op::GT_U, 3), brTable(2),
            CONST(1), bare(Op::BLOCK_END),
            CONST(2), bare(Op::BLOCK_END),
        bare(Op::RETURN),
    };
    uint32_t argCounts[] = {0};
    uint8_t bodyBytes[32];
    Proc proc = makeProc(0, body, sizeof(body) / sizeof(body[0]), bodyBytes, sizeof(bodyBytes));
    uint16_t buf[32];
    TranslateResult r = translateProc(proc, 0, argCounts, 1, buf, 32);
    CHECK(!r.overflowed);
    CHECK(r.halfwordCount == 15);
}

TEST(ComparisonMaterializesAsOrdinaryValue)
{
    // No BR_TABLE/LOOP-exit right after it — takes the
    // materializeComparison path, not fusion.
    const Instr body[] = {CONST(5), opImm(Op::GT_U, 3), bare(Op::RETURN)};
    uint32_t argCounts[] = {0};
    uint8_t bodyBytes[16];
    Proc proc = makeProc(0, body, sizeof(body) / sizeof(body[0]), bodyBytes, sizeof(bodyBytes));
    uint16_t buf[32];
    TranslateResult r = translateProc(proc, 0, argCounts, 1, buf, 32);
    CHECK(!r.overflowed);
    CHECK(r.halfwordCount == 15);
}

TEST(NegViaFullPipeline)
{
    const Instr body[] = {CONST(5), bare(Op::NEG), bare(Op::RETURN)};
    uint32_t argCounts[] = {0};
    uint8_t bodyBytes[8];
    Proc proc = makeProc(0, body, sizeof(body) / sizeof(body[0]), bodyBytes, sizeof(bodyBytes));
    uint16_t buf[32];
    TranslateResult r = translateProc(proc, 0, argCounts, 1, buf, 32);
    CHECK(!r.overflowed);
    CHECK(r.halfwordCount == 11);
}

TEST(ClzHelperViaFullPipeline)
{
    // Reaches emitUnary's own MOV/LDR/BLX-through-helper-vector sequence
    // into clzHelper (runtime/runtime.S) — that sequence's shape is
    // unit-tested directly in test_unaryops.cpp; this is the
    // caller-side wiring around it.
    const Instr body[] = {CONST(5), bare(Op::CLZ), bare(Op::RETURN)};
    uint32_t argCounts[] = {0};
    uint8_t bodyBytes[8];
    Proc proc = makeProc(0, body, sizeof(body) / sizeof(body[0]), bodyBytes, sizeof(bodyBytes));
    uint16_t buf[32];
    TranslateResult r = translateProc(proc, 0, argCounts, 1, buf, 32);
    CHECK(!r.overflowed);
    CHECK(r.halfwordCount == 14);
}

TEST(LastArgumentFoldFallsBackToEagerFlushWhenReferencedTwice)
{
    // Two references to the last argument's slot — takes the fallback
    // path (unconditional flush into physReg(argCount-1)), not the
    // elision TranslateProc1Callee exercises.
    const Instr body[] = {LOAD(0), LOAD(0), opReg(Op::ADD, 0), bare(Op::RETURN)};
    uint32_t argCounts[] = {1};
    uint8_t bodyBytes[16];
    Proc proc = makeProc(1, body, sizeof(body) / sizeof(body[0]), bodyBytes, sizeof(bodyBytes));
    uint16_t buf[32];
    TranslateResult r = translateProc(proc, 0, argCounts, 1, buf, 32);
    CHECK(!r.overflowed);
    CHECK(r.halfwordCount == 11);
}

TEST(CaseClosesViaTerminatorThroughFullPipeline)
{
    // A non-last case closing via bare RETURN, dispatched through
    // translateProc's own closeFrameForTerminator — blocks.h's
    // closeCaseViaTerminator is unit-tested directly (test_blocks.cpp),
    // this is the caller-side switch dispatch around it.
    const Instr body[] = {
        CONST(5), opImm(Op::GT_U, 3), brTable(2),
            CONST(1), bare(Op::RETURN),
            CONST(2), bare(Op::BLOCK_END),
        bare(Op::RETURN),
    };
    uint32_t argCounts[] = {0};
    uint8_t bodyBytes[32];
    Proc proc = makeProc(0, body, sizeof(body) / sizeof(body[0]), bodyBytes, sizeof(bodyBytes));
    uint16_t buf[32];
    TranslateResult r = translateProc(proc, 0, argCounts, 1, buf, 32);
    CHECK(!r.overflowed);
    CHECK(r.halfwordCount == 17);
}

// The tests below cover the remaining paths in translate_proc.cpp's main
// dispatch switch: POP, TRAP (both at the top level and inside an open
// block), out-of-window LOAD/STORE, a STORE not preceded by a foldable
// producer, CONST's large-immediate/fold-into-STORE paths, REG_REG
// (opRegWriteback) both in- and out-of-window, POP_ACC/PEEK_PEEK stack
// combos, a LOOP body that closes via a bare RETURN instead of BLOCK_END,
// and REVBITS's helper-vector call (mirrored after ClzHelperViaFullPipeline
// above) — the ordinary, "not the fused/aligned case" side of paths this
// file's other tests already cover the fused side of.

TEST(PopThroughFullPipeline)
{
    const Instr body[] = {CONST(5), PUSH(), POP(), bare(Op::RETURN)};
    uint32_t argCounts[] = {0};
    uint8_t bodyBytes[16];
    Proc proc = makeProc(0, body, sizeof(body) / sizeof(body[0]), bodyBytes, sizeof(bodyBytes));
    uint16_t buf[32];
    TranslateResult r = translateProc(proc, 0, argCounts, 1, buf, 32);
    CHECK(!r.overflowed);
    CHECK(r.halfwordCount == 11);
}

TEST(TrapAtTopLevel)
{
    const Instr body[] = {trapInstr(3)};
    uint32_t argCounts[] = {0};
    uint8_t bodyBytes[8];
    Proc proc = makeProc(0, body, 1, bodyBytes, sizeof(bodyBytes));
    uint16_t buf[32];
    TranslateResult r = translateProc(proc, 0, argCounts, 1, buf, 32);
    CHECK(!r.overflowed);
    CHECK(r.halfwordCount == 14);
}

TEST(TrapInsideCaseClosesItAndContinuesToNextCase)
{
    // Same shape as CaseClosesViaTerminatorThroughFullPipeline above, but
    // with TRAP as the terminator instead of RETURN — closeFrameForTerminator's
    // "frame != nullptr" guard around TRAP is a distinct source line from
    // RETURN's identically-shaped guard.
    const Instr body[] = {
        CONST(5), opImm(Op::GT_U, 3), brTable(2),
            CONST(1), trapInstr(9),
            CONST(2), bare(Op::BLOCK_END),
        bare(Op::RETURN),
    };
    uint32_t argCounts[] = {0};
    uint8_t bodyBytes[32];
    Proc proc = makeProc(0, body, sizeof(body) / sizeof(body[0]), bodyBytes, sizeof(bodyBytes));
    uint16_t buf[32];
    TranslateResult r = translateProc(proc, 0, argCounts, 1, buf, 32);
    CHECK(!r.overflowed);
    CHECK(r.halfwordCount == 21);
}

TEST(LoadFromOutOfWindowSlot)
{
    // argCount=5 > WINDOW_SIZE(4) — slot 0 is spilled from procedure
    // entry, so LOAD(0) must reload it via ldrSp instead of reading a
    // window register directly.
    const Instr body[] = {LOAD(0), bare(Op::RETURN)};
    uint32_t argCounts[] = {5};
    uint8_t bodyBytes[8];
    Proc proc = makeProc(5, body, 2, bodyBytes, sizeof(bodyBytes));
    uint16_t buf[32];
    TranslateResult r = translateProc(proc, 0, argCounts, 1, buf, 32);
    CHECK(!r.overflowed);
    CHECK(r.halfwordCount == 11);
}

TEST(StoreStandaloneInWindowWhenNotPrecededByAFoldableProducer)
{
    // POP doesn't peek-fold a following STORE (only CONST/LOAD/unary/
    // arithmetic do) — this is the switch's own bare `case Op::STORE`
    // dispatch, not the far-more-common fold path this file's other
    // tests exercise via peekStoreFold.
    const Instr body[] = {CONST(9), PUSH(), POP(), STORE(0), bare(Op::RETURN)};
    uint32_t argCounts[] = {1};
    uint8_t bodyBytes[16];
    Proc proc = makeProc(1, body, sizeof(body) / sizeof(body[0]), bodyBytes, sizeof(bodyBytes));
    uint16_t buf[32];
    TranslateResult r = translateProc(proc, 0, argCounts, 1, buf, 32);
    CHECK(!r.overflowed);
    CHECK(r.halfwordCount == 14);
}

TEST(StoreStandaloneOutOfWindowAndLastArgFoldRefCountZero)
{
    // Two gaps at once: peekStoreFold never folds an out-of-window STORE
    // target regardless of what precedes it (fold.reg is a physical
    // register, meaningless for a spilled slot), so NEG's own STORE(0)
    // here always takes the standalone out-of-window path. Separately,
    // argCount=5's own last argument (slot 4) is never referenced by this
    // body at all — translateProc's own last-argument-fold scan finds
    // refCount==0 and leaves the incoming value as a plain ACC_REG
    // producer, the one case last-arg-fold's own dedicated tests above
    // don't otherwise reach (they all reference the last slot).
    const Instr body[] = {bare(Op::NEG), STORE(0), bare(Op::RETURN)};
    uint32_t argCounts[] = {5};
    uint8_t bodyBytes[8];
    Proc proc = makeProc(5, body, 3, bodyBytes, sizeof(bodyBytes));
    uint16_t buf[32];
    TranslateResult r = translateProc(proc, 0, argCounts, 1, buf, 32);
    CHECK(!r.overflowed);
    CHECK(r.halfwordCount == 12);
}

TEST(ConstTooLargeForImm8SynthesizesInsteadOfStayingPending)
{
    const Instr body[] = {CONST(1000), bare(Op::RETURN)};
    uint32_t argCounts[] = {0};
    uint8_t bodyBytes[8];
    Proc proc = makeProc(0, body, 2, bodyBytes, sizeof(bodyBytes));
    uint16_t buf[32];
    TranslateResult r = translateProc(proc, 0, argCounts, 1, buf, 32);
    CHECK(!r.overflowed);
    CHECK(r.halfwordCount == 12);
}

TEST(ConstFoldsDirectlyIntoAFollowingStore)
{
    const Instr body[] = {CONST(5), STORE(0), bare(Op::RETURN)};
    uint32_t argCounts[] = {1};
    uint8_t bodyBytes[8];
    Proc proc = makeProc(1, body, 3, bodyBytes, sizeof(bodyBytes));
    uint16_t buf[32];
    TranslateResult r = translateProc(proc, 0, argCounts, 1, buf, 32);
    CHECK(!r.overflowed);
    CHECK(r.halfwordCount == 12);
}

TEST(RegRegOutOfWindowWritesBackToStackAfterComputing)
{
    // REG_REG (opRegWriteback) with an out-of-window target: the operand
    // reload (shared with REG_ACC) and REG_REG's own SCRATCH_REG-as-dest
    // plus explicit str-back are both otherwise only exercised indirectly
    // through binops.cpp's own lower-level tests, never through this
    // file's real dispatch.
    const Instr body[] = {opRegWriteback(Op::ADD, 0), CONST(1), bare(Op::RETURN)};
    uint32_t argCounts[] = {5};
    uint8_t bodyBytes[16];
    Proc proc = makeProc(5, body, 3, bodyBytes, sizeof(bodyBytes));
    uint16_t buf[32];
    TranslateResult r = translateProc(proc, 0, argCounts, 1, buf, 32);
    CHECK(!r.overflowed);
    CHECK(r.halfwordCount == 14);
}

TEST(RegRegInWindowWritesBackToItsOwnRegister)
{
    const Instr body[] = {CONST(5), PUSH(), opRegWriteback(Op::ADD, 0), LOAD(0), bare(Op::RETURN)};
    uint32_t argCounts[] = {0};
    uint8_t bodyBytes[16];
    Proc proc = makeProc(0, body, sizeof(body) / sizeof(body[0]), bodyBytes, sizeof(bodyBytes));
    uint16_t buf[32];
    TranslateResult r = translateProc(proc, 0, argCounts, 1, buf, 32);
    CHECK(!r.overflowed);
    CHECK(r.halfwordCount == 12);
}

TEST(PopAccStackComboThroughFullPipeline)
{
    const Instr body[] = {CONST(3), PUSH(), CONST(2), opStack(Op::ADD, Combo::POP_ACC), bare(Op::RETURN)};
    uint32_t argCounts[] = {0};
    uint8_t bodyBytes[16];
    Proc proc = makeProc(0, body, sizeof(body) / sizeof(body[0]), bodyBytes, sizeof(bodyBytes));
    uint16_t buf[32];
    TranslateResult r = translateProc(proc, 0, argCounts, 1, buf, 32);
    CHECK(!r.overflowed);
    CHECK(r.halfwordCount == 11);
}

TEST(PeekPeekStackComboThroughFullPipeline)
{
    const Instr body[] = {CONST(3), PUSH(), CONST(6), opStack(Op::AND, Combo::PEEK_PEEK), POP(), bare(Op::RETURN)};
    uint32_t argCounts[] = {0};
    uint8_t bodyBytes[16];
    Proc proc = makeProc(0, body, sizeof(body) / sizeof(body[0]), bodyBytes, sizeof(bodyBytes));
    uint16_t buf[32];
    TranslateResult r = translateProc(proc, 0, argCounts, 1, buf, 32);
    CHECK(!r.overflowed);
    CHECK(r.halfwordCount == 14);
}

TEST(LoopBodyClosesViaTerminatorInsteadOfBlockEnd)
{
    // An empty condition sub-block (testAccNonzero is the whole test),
    // then a body closed via a bare RETURN rather than BLOCK_END —
    // exercises closeFrameForTerminator's LoopBody dispatch
    // (closeLoopBodyViaTerminator), not the Case dispatch the other
    // terminator-close tests exercise. There is no way to reach that
    // dispatch with frame.kind == LoopCond: a loop's condition sub-block
    // can only close via BLOCK_END, so closeLoopBodyViaTerminator's own
    // frame.kind == LoopBody assert is genuinely unreachable, not merely
    // untested (hence its GCOV_EXCL_LINE).
    const Instr body[] = {
        bare(Op::LOOP), bare(Op::BLOCK_END),
        CONST(42), bare(Op::RETURN),
        CONST(999), bare(Op::RETURN),
    };
    uint32_t argCounts[] = {0};
    uint8_t bodyBytes[16];
    Proc proc = makeProc(1, body, sizeof(body) / sizeof(body[0]), bodyBytes, sizeof(bodyBytes));
    uint16_t buf[32];
    TranslateResult r = translateProc(proc, 0, argCounts, 1, buf, 32);
    CHECK(!r.overflowed);
    CHECK(r.halfwordCount == 18);
}

TEST(RevbitsHelperViaFullPipeline)
{
    // Mirrors ClzHelperViaFullPipeline above, for REVBITS's own helper
    // vector index (revbitsHelper, index 5).
    const Instr body[] = {CONST(1), bare(Op::REVBITS), bare(Op::RETURN)};
    uint32_t argCounts[] = {0};
    uint8_t bodyBytes[8];
    Proc proc = makeProc(0, body, 3, bodyBytes, sizeof(bodyBytes));
    uint16_t buf[32];
    TranslateResult r = translateProc(proc, 0, argCounts, 1, buf, 32);
    CHECK(!r.overflowed);
    CHECK(r.halfwordCount == 14);
}

TEST(ComparisonImmediatelyBeforeBrTableJumpTableDoesNotFuse)
{
    // Fusion only applies for BR_TABLE N<=2 (if/if-else) — a comparison
    // right before a genuine N>2 jump-table selector must materialize
    // as an ordinary 0/1 value instead, exercising the "hasLookahead but
    // op doesn't qualify" side of fusesIntoBrTable's own condition.
    const Instr body[] = {
        LOAD(0), opImm(Op::GT_U, 3), brTable(3),
            CONST(10), bare(Op::BLOCK_END),
            CONST(20), bare(Op::BLOCK_END),
            CONST(30), bare(Op::BLOCK_END),
        bare(Op::RETURN),
    };
    uint32_t argCounts[] = {1};
    uint8_t bodyBytes[32];
    Proc proc = makeProc(1, body, sizeof(body) / sizeof(body[0]), bodyBytes, sizeof(bodyBytes));
    uint16_t buf[64];
    TranslateResult r = translateProc(proc, 0, argCounts, 1, buf, 64);
    CHECK(!r.overflowed);
    CHECK(r.halfwordCount == 28);
}

TEST(LoopConditionClosesViaAnExplicitFusedComparison)
{
    // The other half of LoopClosesNormallyViaBlockEndBackEdge's own
    // sibling: that test's condition relies on testAccNonzero's fallback
    // (a bare LOAD with no comparison) — this one has a real comparison
    // immediately before the condition's own BLOCK_END, hitting
    // fusesIntoLoopExit rather than the fallback.
    const Instr body[] = {
        CONST(3), PUSH(),
        bare(Op::LOOP),
            LOAD(0), opImm(Op::GT_S, 0),
        bare(Op::BLOCK_END),
            LOAD(0), opImm(Op::SUB, 1), STORE(0),
        bare(Op::BLOCK_END),
        bare(Op::RETURN),
    };
    uint32_t argCounts[] = {0};
    uint8_t bodyBytes[32];
    Proc proc = makeProc(0, body, sizeof(body) / sizeof(body[0]), bodyBytes, sizeof(bodyBytes));
    uint16_t buf[32];
    TranslateResult r = translateProc(proc, 0, argCounts, 1, buf, 32);
    CHECK(!r.overflowed);
    CHECK(r.halfwordCount == 16);
}

TEST(ComparisonMaterializedResultFoldsDirectlyIntoAFollowingStore)
{
    // materializeComparison's own fold.reg>=0 path (the comparison-as-
    // value analog of CONST/arithmetic's own store-fold) — every other
    // comparison-as-value test in this file has the result land in
    // ACC_REG (fold.reg<0) instead.
    const Instr body[] = {CONST(5), opImm(Op::GT_U, 3), STORE(0), bare(Op::RETURN)};
    uint32_t argCounts[] = {1};
    uint8_t bodyBytes[16];
    Proc proc = makeProc(1, body, sizeof(body) / sizeof(body[0]), bodyBytes, sizeof(bodyBytes));
    uint16_t buf[32];
    TranslateResult r = translateProc(proc, 0, argCounts, 1, buf, 32);
    CHECK(!r.overflowed);
    CHECK(r.halfwordCount == 17);
}

TEST(LastArgumentFoldEagerlyFlushesWhenBodyStartReferenceIsNotALoad)
{
    // The last-arg-fold's own LOAD-elision fast path is keyed on the
    // first reference being a body-start *LOAD* specifically — a
    // REG_ACC read (opReg) at the very same body-start position must
    // still take the eager-flush path, since it reads via physReg(slot)
    // rather than acc's own pending value.
    const Instr body[] = {opReg(Op::ADD, 1), bare(Op::RETURN)};
    uint32_t argCounts[] = {2};
    uint8_t bodyBytes[8];
    Proc proc = makeProc(2, body, 2, bodyBytes, sizeof(bodyBytes));
    uint16_t buf[32];
    TranslateResult r = translateProc(proc, 0, argCounts, 1, buf, 32);
    CHECK(!r.overflowed);
    CHECK(r.halfwordCount == 11);
}

namespace
{
uint32_t currentSp()
{
    register uint32_t sp asm("sp");
    return sp;
}
} // namespace

TEST(DeeplyNestedButWellFormedBlocksSucceedWithNoStackFloor)
{
    // 50 levels of well-formed nesting (each BR_TABLE(1) case properly
    // closed by its own BLOCK_END) — deliberately past the old
    // MAX_BLOCK_NESTING(32) constant this replaced, to show concretely
    // that legitimately deep (but not runaway) nesting is no longer
    // rejected just for exceeding an arbitrary count: with the default
    // stackFloor (0, no limit), only the live stack pointer's real
    // headroom matters, and this host process has plenty of it.
    constexpr int kDepth = 50;
    Instr body[2 * kDepth + 1];
    for(int i = 0; i < kDepth; i++)
    {
        body[i] = brTable(1);
    }
    for(int i = 0; i < kDepth; i++)
    {
        body[kDepth + i] = bare(Op::BLOCK_END);
    }
    body[2 * kDepth] = bare(Op::RETURN);
    uint32_t argCounts[] = {0};
    uint8_t bodyBytes[512];
    Proc proc = makeProc(0, body, 2 * kDepth + 1, bodyBytes, sizeof(bodyBytes));
    uint16_t buf[512];
    TranslateResult r = translateProc(proc, 0, argCounts, 1, buf, 512);
    CHECK(!r.overflowed);
}

TEST(BlockNestingReportsOverflowWhenLiveStackFloorIsUnsatisfiable)
{
    // stackFloor pinned at (essentially) the current sp — no margin left
    // at all — so translateBody's very first live check already fails,
    // regardless of how shallow the body is (a bare RETURN, not even one
    // level of nesting). Deliberately doesn't try to calibrate "how many
    // levels of real nesting exhaust N bytes of stack": this host build
    // is -O0, nothing like the real target's -Os, so any such number
    // wouldn't transfer — this instead proves the mechanism itself (the
    // comparison, and TranslateResult::overflowed propagating out) fires
    // correctly whenever the floor genuinely can't be satisfied, which is
    // exactly the property a genuinely deep recursion needs to trigger for
    // real, on real hardware.
    const Instr body[] = {bare(Op::RETURN)};
    uint32_t argCounts[] = {0};
    uint8_t bodyBytes[8];
    Proc proc = makeProc(0, body, 1, bodyBytes, sizeof(bodyBytes));
    uint16_t buf[16];
    uint32_t floor = currentSp();
    TranslateResult r = translateProc(proc, 0, argCounts, 1, buf, 16, floor);
    CHECK(r.overflowed);
}
