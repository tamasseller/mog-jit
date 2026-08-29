// Expected halfwords are cross-checked against arm-none-eabi-as, not
// re-derived from the same formulas under test.
#include "Test.h"
#include "Mock.h"
#include "assembler.h"
#include "window.h"
#include "accstate.h"
#include "shape.h"
#include "armv6.h"

#include "runtime_internal.h"
#include "host_runtime_support.h"

#include <initializer_list>

using namespace jitc;

TEST(physRegIsDescendingAndWrapsAtWindowSize)
{
    CHECK(physReg(0) == 7 && physReg(1) == 6 && physReg(2) == 5 && physReg(3) == 4);
    CHECK(physReg(4) == 7 && physReg(5) == 6 && physReg(6) == 5 && physReg(7) == 4); // wraps
}

TEST(spillOffsetIsClosestSpillClosestToSp)
{
    Window w6(6);
    CHECK(w6.spillOffset(0) == 4); // 2 spilled (k=0,1); k=0 spilled first, furthest from sp
    CHECK(w6.spillOffset(1) == 0); // k=1 spilled last, closest to sp
    Window w5(5);
    CHECK(w5.spillOffset(0) == 0); // exactly 1 spilled
}

TEST(spillOffsetGetsSavesLRAdjustmentOnlyForOriginalOutOfWindowArgs)
{
    // argCount=5 > WINDOW_SIZE(4): k=0 is the one original out-of-window
    // argument (initialSpilledCount=1) — a caller placed it before this
    // procedure's own prologue ran. Not adjusted at all when this
    // procedure is a leaf (no push{lr} sits between the caller's
    // placement and this procedure's own reads).
    Window leaf(5, /*savesLR=*/false);
    CHECK(leaf.spillOffset(0) == 0); // 1 spilled, closest to sp

    // Adjusted by +4 when this procedure is non-leaf: its own push{lr}
    // (protecting the incoming record before a nested CALL can clobber
    // it) landed strictly between the caller's placement of k=0 and this
    // procedure's own first read of it.
    Window nonLeaf(5, /*savesLR=*/true);
    CHECK(nonLeaf.spillOffset(0) == 0 + 4);

    // A local this procedure spills *itself*, later, via its own PUSH —
    // k=1 here — needs no adjustment even though it's non-leaf: it was
    // spilled strictly after the prologue's own push{lr}, so this
    // procedure's own view of sp is already self-consistent for it.
    TestAssembler e_ta(4);
    Assembler &e = e_ta.a;
    const uint16_t *buf = e_ta.code();
    AccState acc;
    acc.producer(Shape::ofImm(99));
    nonLeaf.pushValue(e, acc); // tos: 5 -> 6, evicts k=1 to the real stack
    CHECK(nonLeaf.spillOffset(0) == 4 + 4); // k=0 still adjusted (raw offset grew with tos)
    CHECK(nonLeaf.spillOffset(1) == 0);     // k=1 (self-spilled) — not adjusted
}

TEST(pushValueEvictsAtWindowBoundary)
{
    TestAssembler e_ta(16);
    Assembler &e = e_ta.a;
    const uint16_t *buf = e_ta.code();
    Window w(0);
    AccState acc;
    int values[] = {10, 20, 30, 40, 50};
    for(int v : values)
    {
        acc.producer(Shape::ofImm(v));
        w.pushValue(e, acc);
    }

    CHECK(e.halfwordCount() == 6);
    CHECK(buf[0] == ArmV6M::movs(ArmV6M::LoReg(7), ArmV6M::Imm<8>(10))); // MOVS r7, #10  (k=0 -> physReg(0)=r7)
    CHECK(buf[1] == ArmV6M::movs(ArmV6M::LoReg(6), ArmV6M::Imm<8>(20))); // MOVS r6, #20  (k=1 -> r6)
    CHECK(buf[2] == ArmV6M::movs(ArmV6M::LoReg(5), ArmV6M::Imm<8>(30))); // MOVS r5, #30  (k=2 -> r5)
    CHECK(buf[3] == ArmV6M::movs(ArmV6M::LoReg(4), ArmV6M::Imm<8>(40))); // MOVS r4, #40  (k=3 -> r4, window now full)
    CHECK(buf[4] == ArmV6M::push(ArmV6M::LoRegs{0}.add(ArmV6M::LoReg(7)))); // PUSH {r7}     (k=4 evicts k=0's r7)
    CHECK(buf[5] == ArmV6M::movs(ArmV6M::LoReg(7), ArmV6M::Imm<8>(50))); // MOVS r7, #50  (k=4 lands back on r7)
    CHECK(w.tos == 5);
}

TEST(finishPopReloadsWhatPushEvicted)
{
    TestAssembler e_ta(16);
    Assembler &e = e_ta.a;
    const uint16_t *buf = e_ta.code();
    Window w(0);
    AccState acc;
    for(int v : {10, 20, 30, 40, 50})
    {
        acc.producer(Shape::ofImm(v));
        w.pushValue(e, acc);
    }
    uint32_t before = e.halfwordCount();

    w.finishPop(e); // pops the top slot (tos=5 -> 4); must reload k=0's spilled r7
    CHECK(e.halfwordCount() == before + 1);
    CHECK(buf[before] == ArmV6M::pop(ArmV6M::LoRegs{0}.add(ArmV6M::LoReg(7)))); // POP {r7}
    CHECK(w.tos == 4);
}

TEST(discardWindowIsOneBareSpAdjustment)
{
    TestAssembler e_ta(4);
    Assembler &e = e_ta.a;
    const uint16_t *buf = e_ta.code();
    Window w(6); // 2 slots spilled (tos=6, WINDOW_SIZE=4), leaf
    w.discard(e);
    CHECK(e.halfwordCount() == 1);
    CHECK(buf[0] == ArmV6M::incrSp(ArmV6M::Uoff<2, 7>(8))); // ADD sp, #8
}

TEST(discardWindowForSavesLRReclaimsOnlySelfSpilledLocals)
{
    // argCount=5 (initialSpilledCount=1) — a non-leaf procedure's own
    // discardWindow must not reclaim that one original out-of-window
    // argument: it sits below this procedure's own push{lr}, which
    // abiEmitReturn (not discardWindow) reclaims after retrieving the
    // saved record.
    TestAssembler e_ta(4);
    Assembler &e = e_ta.a;
    const uint16_t *buf = e_ta.code();
    Window w(5, /*savesLR=*/true); // tos=5, spilledCount=1, all of it "original"
    w.discard(e);
    CHECK(e.halfwordCount() == 0); // nothing self-spilled — nothing to reclaim here
}

TEST(discardWindowAcceptsTheMaxEncodableSpAdjustment)
{
    TestAssembler e_ta(4);
    Assembler &e = e_ta.a;
    const uint16_t *buf = e_ta.code();
    Window w(4 + 127); // 127 words spilled -> 508 bytes, exactly Uoff<2,7>::maxValue
    w.discard(e);
    CHECK(e.halfwordCount() == 1);
    CHECK(buf[0] == ArmV6M::incrSp(ArmV6M::Uoff<2, 7>(508)));
}

TEST(discardWindowBailsWhenTheSpAdjustmentExceedsTheEncodableRange)
{
    // F6: incrSp(Uoff<2,7>(...)) used to silently flip ADD into SUB
    // (fmtImm7's unmasked OR bleeds into the opcode's own ADD/SUB bit)
    // instead of failing once more than 127 words are spilled.
    TestAssembler e_ta(4);
    Assembler &e = e_ta.a;
    const uint16_t *buf = e_ta.code();
    Window w(4 + 128); // 128 words spilled -> 512 bytes, one word past the limit
    CHECK(!w.discard(e));
}

TEST(restoreWindowBailsWhenTheSpAdjustmentExceedsTheEncodableRange)
{
    TestAssembler e_ta(4);
    Assembler &e = e_ta.a;
    const uint16_t *buf = e_ta.code();
    Window w(4 + 128);
    CHECK(!w.restore(e, 0));
}

TEST(callShuffleWithStackArgsExceedingWindowSize)
{
    // 6 stack-passed args (WINDOW_SIZE=4), caller's window fully occupied by
    // them (tos=6, nothing else resident) — exactly the scenario
    // fillCalleeArgs's WINDOW_SIZE-1 cap (not WINDOW_SIZE) exists for; a
    // wrong cap here would silently reassign which value lands in which
    // register.
    TestAssembler e1_ta(8);
    Assembler &e1 = e1_ta.a;
    const uint16_t *buf1 = e1_ta.code();
    Window w(6);
    w.spillForCall(e1, 6);
    CHECK(e1.halfwordCount() == 2);
    CHECK(buf1[0] == ArmV6M::push(ArmV6M::LoRegs{0}.add(ArmV6M::LoReg(4)).add(ArmV6M::LoReg(5)))); // PUSH {r4, r5}  (pre-wrap run, k=2,3)
    CHECK(buf1[1] == ArmV6M::push(ArmV6M::LoRegs{0}.add(ArmV6M::LoReg(6)).add(ArmV6M::LoReg(7)))); // PUSH {r6, r7}  (post-wrap run, k=4,5)

    TestAssembler e2_ta(8);
    Assembler &e2 = e2_ta.a;
    const uint16_t *buf2 = e2_ta.code();
    Window::fillCalleeArgs(e2, 6);
    CHECK(e2.halfwordCount() == 2);
    CHECK(buf2[0] == ArmV6M::pop(ArmV6M::LoRegs{0}.add(ArmV6M::LoReg(6)).add(ArmV6M::LoReg(7)))); // POP {r6, r7}  (larger-k run first)
    CHECK(buf2[1] == ArmV6M::pop(ArmV6M::LoRegs{0}.add(ArmV6M::LoReg(4)))); // POP {r4}      (k=3's own lone run)

    TestAssembler e3_ta(8);
    Assembler &e3 = e3_ta.a;
    const uint16_t *buf3 = e3_ta.code();
    w.reloadAfterCall(e3, 0); // targetTos = tos(6) - stackArgs(6) = 0
    CHECK(e3.halfwordCount() == 0); // nothing left over to restore
    CHECK(w.tos == 0);
}

TEST(callShuffleWithLeftoverLocalsAboveTheStackArgs)
{
    // tos=5 but only the *closest* 2 slots (k=3,4) are this call's own stack
    // args — k=1,2 are leftover locals the caller still needs after the
    // call returns, so spillForCall's own leading plain-PUSH branch (base >
    // bottom) fires to preserve them, distinct from pushLargestKClosest's
    // own per-argument pushes just below.
    TestAssembler e_ta(8);
    Assembler &e = e_ta.a;
    const uint16_t *buf = e_ta.code();
    Window w(5);
    w.spillForCall(e, 2);
    CHECK(e.halfwordCount() == 3);
    CHECK(buf[0] == ArmV6M::push(ArmV6M::LoRegs{0}.add(ArmV6M::LoReg(5)).add(ArmV6M::LoReg(6)))); // PUSH {r5, r6}  (leftover locals k=1,2, one bulk push)
    CHECK(buf[1] == ArmV6M::push(ArmV6M::LoRegs{0}.add(ArmV6M::LoReg(4)))); // PUSH {r4}      (stack arg k=3)
    CHECK(buf[2] == ArmV6M::push(ArmV6M::LoRegs{0}.add(ArmV6M::LoReg(7)))); // PUSH {r7}      (stack arg k=4, closest to sp)
}
