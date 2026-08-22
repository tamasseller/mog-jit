// The anchor test: compiles jit-armv6m/prototype/test/call.test.ts's
// program 1 ("single-argument call, entirely acc-passed") end to end and
// asserts the *entire* emitted halfword array for both procedures against
// literals hand-derived from the ARMv6-M encoding tables and independently
// cross-checked against arm-none-eabi-as — proving the whole pipeline
// (bytecode -> Emitter/Window/AccState/binops/abi_strategy) composes
// correctly, fast, without QEMU. The QEMU fixture for this same program
// (test/qemu/fixtures.cpp, fixture #1) is the behavioral proof that these
// bytes, executed for real against the real dispatch/eviction runtime,
// actually produce 42.
#include "Test.h"
#include "translate_proc.h"

using namespace jitc;

namespace {
// proc0 (argCount 0): CONST(37), call(1), RETURN
const Instr kProc0Body[] = {CONST(37), call(1), bare(Op::RETURN)};
// proc1 (argCount 1): LOAD(0), opImm(ADD, 5), RETURN
const Instr kProc1Body[] = {LOAD(0), opImm(Op::ADD, 5), bare(Op::RETURN)};
const uint32_t kArgCounts[] = {0, 1};
}

TEST(TranslateProc0EntryProcedure)
{
    // proc0 makes a CALL, so it's non-leaf (savesLR): its prologue gains
    // push{lr}, and — because that shifts preCallPc by 2 bytes — the
    // record's own resume offset (k) converges one step further than the
    // pre-redesign encoding (offsetPlus1=19=0x13, not 17=0x11), and its
    // RETURN dispatches through returnHelperFromStack (index 2, offset 8)
    // instead of the old single returnHelper. argCount=0 keeps
    // initialSpilledCount at 0, so this is still the ordinary non-leaf
    // case, not the rare inline-pop-and-reclaim one.
    uint16_t buf[32];
    Proc proc{0, kProc0Body, 3};
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
    for(uint32_t i = 0; i < r.halfwordCount; i++) CHECK(buf[i] == expected[i]);
}

TEST(TranslateProc1Callee)
{
    uint16_t buf[32];
    Proc proc{1, kProc1Body, 3};
    TranslateResult r = translateProc(proc, /*procIdx=*/1, kArgCounts, 2, buf, 32);

    CHECK(!r.overflowed);
    CHECK(r.halfwordCount == 11);

    const uint16_t expected[] = {
        0x465B, 0x604B, 0x3301, 0x469B, 0x447A, 0x4710, // prologue stub
        0x4607,                                          // MOV r7, r0  (callee-side: last/only arg arrives in acc, lands at physReg(0)=r7)
        0x1D78,                                            // ADDS r0, r7, #5  (LOAD(0)+opImm(ADD,5) folded straight into acc)
        0x4653, 0x685B, 0x4718,                             // returnHelper tail
    };
    for(uint32_t i = 0; i < r.halfwordCount; i++) CHECK(buf[i] == expected[i]);
}

TEST(OverflowIsReportedRatherThanOverrunningTheBuffer)
{
    uint16_t buf[4]; // too small for even the 6-halfword prologue alone
    Proc proc{0, kProc0Body, 3};
    TranslateResult r = translateProc(proc, 0, kArgCounts, 2, buf, 4);
    CHECK(r.overflowed);
}
