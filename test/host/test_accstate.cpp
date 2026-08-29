#include "Test.h"
#include "assembler.h"
#include "accstate.h"
#include "shape.h"
#include "armv6.h"

#include "host_runtime_support.h"

using namespace jitc;

TEST(startsCleanInAccReg)
{
    AccState acc;
    Shape s = acc.peek();
    CHECK(!s.isImm && s.reg == ACC_REG);
}

TEST(producerThenPeekReturnsPendingShapeUnmaterialized)
{
    TestAssembler e_ta(4);
    Assembler &e = e_ta.a;
    const uint16_t *buf = e_ta.code();
    AccState acc;
    acc.producer(Shape::ofImm(42));
    CHECK(e.halfwordCount() == 0); // producer alone never emits
    Shape s = acc.peek();
    CHECK(s.isImm && s.imm == 42);
    CHECK(e.halfwordCount() == 0); // peek doesn't discharge it either
}

TEST(flushMaterializesPendingAndBecomesClean)
{
    TestAssembler e_ta(4);
    Assembler &e = e_ta.a;
    const uint16_t *buf = e_ta.code();
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
    TestAssembler e_ta(4);
    Assembler &e = e_ta.a;
    const uint16_t *buf = e_ta.code();
    AccState acc; // starts Clean(ACC_REG)
    acc.flush(e, ACC_REG);
    CHECK(e.halfwordCount() == 0);
}

TEST(flushLiveOnPoisonedAccIsANoOp)
{
    // The ordinary case at a control-flow merge point
    // (translate_proc.cpp's localJumpCleanup and translateLoop's own
    // entry): isa-core.md §8.7 poisons acc at every CFG split/merge
    // unconditionally, not just when the last instruction in a case/loop
    // body happened to clobber it via REG_REG/PEEK_PEEK. flushLive treats
    // this as a no-op, not an error — nothing downstream could read acc
    // either way.
    TestAssembler e_ta(4);
    Assembler &e = e_ta.a;
    const uint16_t *buf = e_ta.code();
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
