#include "Test.h"
#include "assembler.h"
#include "accstate.h"
#include "shape.h"
#include "armv6.h"

using namespace jitc;

TEST(startsCleanInAccReg)
{
    AccState acc;
    Shape s = acc.peek();
    CHECK(!s.isImm && s.reg == ACC_REG);
}

TEST(producerThenPeekReturnsPendingShapeUnmaterialized)
{
    uint16_t buf[4];
    Assembler e(buf, 4);
    AccState acc;
    acc.producer(Shape::ofImm(42));
    CHECK(e.halfwordCount() == 0); // producer alone never emits
    Shape s = acc.peek();
    CHECK(s.isImm && s.imm == 42);
    CHECK(e.halfwordCount() == 0); // peek doesn't discharge it either
}

TEST(flushMaterializesPendingAndBecomesClean)
{
    uint16_t buf[4];
    Assembler e(buf, 4);
    AccState acc;
    acc.producer(Shape::ofImm(3));
    acc.flush(e, 5);
    CHECK(e.halfwordCount() == 1);
    CHECK(buf[0] == ArmV6M::movs(ArmV6M::LoReg(5), ArmV6M::Imm<8>(3))); // MOVS r5, #3
    Shape s = acc.peek();
    CHECK(!s.isImm && s.reg == 5);
}

TEST(flushOfAlreadyCleanSameRegisterIsANoOp)
{
    uint16_t buf[4];
    Assembler e(buf, 4);
    AccState acc; // starts Clean(ACC_REG)
    acc.flush(e, ACC_REG);
    CHECK(e.halfwordCount() == 0);
}

TEST(flushLiveOnPoisonedAccIsANoOp)
{
    // The one legitimate case where acc is poisoned at a control-flow merge
    // (blocks.h's closeBlockEnd/closeCaseViaTerminator): the last
    // instruction in a case/loop body clobbered acc (REG_REG or PEEK_PEEK)
    // with nothing after to re-establish it. flushLive treats this as a
    // no-op, not an error — nothing downstream could read acc either way.
    uint16_t buf[4];
    Assembler e(buf, 4);
    AccState acc;
    acc.poison();
    acc.flushLive(e, ACC_REG);
    CHECK(e.halfwordCount() == 0);
}

TEST(setCleanThenPoisonThenProducerSupersedes)
{
    AccState acc;
    acc.setClean(7);
    Shape s = acc.peek();
    CHECK(!s.isImm && s.reg == 7);

    acc.poison();
    // peek()/flush() on a poisoned state would assert (a translator-logic
    // bug, never legitimate input) — not exercised here.

    acc.producer(Shape::ofReg(2));
    Shape s2 = acc.peek(); // producer supersedes Poisoned without issue
    CHECK(!s2.isImm && s2.reg == 2);
}
