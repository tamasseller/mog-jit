// Compiles a two-procedure program ("single-argument call, entirely
// acc-passed") end to end and checks the entire emitted halfword array
// against literals hand-derived from the ARMv6-M encoding tables and
// cross-checked against arm-none-eabi-as — proves the whole pipeline
// (bytecode -> Assembler/Window/AccState/binops/abi_strategy) composes
// correctly without QEMU.
#include "Test.h"
#include "translate_proc.h"
#include "ext.h"
#include "ext_stub.h"
#include "encode_instr.h"
#include "armv6.h"
#include "registers.h"

#include "runtime.h"
#include "dispatch_abi.h"
#include "host_runtime_support.h"

using namespace jitc;


static constexpr uint32_t PC = 15;

// The 2-halfword entry stub every compiled procedure begins with
// (emitPrologueStub): a pc-relative ADD into the body, nothing else.
// Never varies with procIdx, argCount, or body content.
#define PROLOGUE_STUB \
    ArmV6M::add(ArmV6M::AnyReg(ENTRY_OFFSET_REG), ArmV6M::AnyReg(PC)), \
    ArmV6M::bx(ArmV6M::AnyReg(ENTRY_OFFSET_REG)) /* prologue stub */

// abiEmitReturn's tail when the procedure saved lr on entry (savesLR,
// initialSpilledCount == 0): dispatches through returnHelperFromStack
// (helper-vector index 2, offset 8).
#define RETURN_VIA_STACK \
    ArmV6M::mov(ArmV6M::AnyReg(ENTRY_JUMP_REG), ArmV6M::AnyReg(HELPER_VEC_REG)), \
    ArmV6M::ldr(ArmV6M::LoReg(ENTRY_JUMP_REG), ArmV6M::LoReg(ENTRY_JUMP_REG), ArmV6M::Uoff<2, 5>(8)), \
    ArmV6M::bx(ArmV6M::AnyReg(ENTRY_JUMP_REG)) /* returnHelperFromStack */

// ...and when it didn't save lr: returnHelperFromLr (index 1, offset 4).
#define RETURN_VIA_LR \
    ArmV6M::mov(ArmV6M::AnyReg(ENTRY_JUMP_REG), ArmV6M::AnyReg(HELPER_VEC_REG)), \
    ArmV6M::ldr(ArmV6M::LoReg(ENTRY_JUMP_REG), ArmV6M::LoReg(ENTRY_JUMP_REG), ArmV6M::Uoff<2, 5>(4)), \
    ArmV6M::bx(ArmV6M::AnyReg(ENTRY_JUMP_REG)) /* returnHelperFromLr */

// A TRAP's own dispatch: the same three-instruction helper-vector jump,
// aimed at slot 8 (trapHelper) instead of a return slot. No window
// discard and no record retrieval precede it — trapHelper restores the
// excursion's whole saved sp instead (runtime/runtime.S).
#define TRAP_VIA_HELPER \
    ArmV6M::mov(ArmV6M::AnyReg(ENTRY_JUMP_REG), ArmV6M::AnyReg(HELPER_VEC_REG)), \
    ArmV6M::ldr(ArmV6M::LoReg(ENTRY_JUMP_REG), ArmV6M::LoReg(ENTRY_JUMP_REG), ArmV6M::Uoff<2, 5>(HELPER_TRAP_OFFSET)), \
    ArmV6M::bx(ArmV6M::AnyReg(ENTRY_JUMP_REG)) /* trapHelper */

// proc0 (argCount 0): CONST(37), call(1), RETURN
static const Instr kProc0Body[] = {CONST(37), call(1), bare(Op::RETURN)};
// proc1 (argCount 1): LOAD(0), opImm(ADD, 5), RETURN
static const Instr kProc1Body[] = {LOAD(0), opImm(Op::ADD, 5), bare(Op::RETURN)};

// translateProc reads argCount/bodyPtr/bodyBytes/needsLRSave straight out
// of the Runtime's own ProcSlot, and always compiles through an Assembler
// attached to that Runtime's arena — there is no buffer-only entry point a
// test could hand a throwaway pair to.
//
// That arena, and every ProcSlot's own bodyPtr, are addressed as a bare
// uint32_t — a real target's flat 32-bit address space. A 64-bit host
// process's own real memory doesn't generally fit that (ASLR puts both
// the stack and ordinary heap/static storage above 4GB), so this file
// can't just point those fields at an ordinary local buffer the way the
// old detached Assembler(buf, capacity) could. LowMemory (host_runtime_
// support.h) is the fix: an mmap(..., MAP_32BIT) region — real,
// dereferenceable memory that also happens to live below 4GB, so the
// uint32_t round-trip loses nothing.

// Arbitrary but fixed — none of this file's tests exercise eviction
// ordering (test/qemu's job, plus test_runtime_arena.cpp's own
// findEvictionVictim cases), so the exact tick finalize() stamps into the
// slot never matters here, only that one is supplied.
static constexpr uint32_t LRU_TICK = 1000;

// Every real caller reads a procedure's own argCount/bodyPtr/bodyBytes/
// needsLRSave through its slot in a Runtime, and every CALL site reads
// just the callee's argCount() the same way (translate_proc.cpp) — set()
// below is the procedure-under-test path: it encodes body[count] via
// encodeBody() into fresh LowMemory and registers the result as procIdx's
// own slot. A procedure that's only ever CALL'd, never itself translated
// in a given test, needs no real body at all (body defaults to nullptr).
template<uint32_t procCount>
class FakeRuntime
{
    alignas(8) uint8_t bytes[sizeof(Runtime) + (procCount + 1) * sizeof(ProcSlot)] = {};
    LowMemory low{1u << 20}; // 1 MiB — comfortably covers every test's own arena + bodies
    uint32_t arenaBase;
    CodeArena arena = CodeArena::region(0, 0, /*stackLimit=*/0);
public:
    explicit FakeRuntime(uint32_t arenaBytes = 64, uint32_t stackLimit = 0)
    {
        arenaBase = low.alloc(arenaBytes);
        arena = CodeArena::region(arenaBase, arenaBytes, stackLimit);
        new(bytes) Runtime(procCount, arena);
        // Every slot starts not-resident (Runtime::isResident() reads
        // codePtr against trampolineAddr) — left at its zero-init default
        // otherwise, growForAttached's own findEvictionVictim/evict loop
        // sees a bogus resident procedure (this file's own trampolineAddr,
        // test_runtime_arena.cpp's 0xDEADBEEF, is nonzero) and evicts it,
        // corrupting whichever procedure is actually mid-translation.
        for(uint32_t i = 0; i < procCount; i++)
        {
            runtime().slot(i).codePtr = trampolineAddr;
        }
    }
    Runtime &runtime() { return *reinterpret_cast<Runtime *>(bytes); }

    // Reserves cap bytes of low, dereferenceable memory for procedure
    // idx's own body, pins it as that slot's bodyPtr, and hands back the
    // raw pointer to fill — for the handful of tests that hand-splice an
    // extension opcode encodeBody can't express (extBody below).
    // Ordinary Instr[] bodies go through set() instead.
    uint8_t *bodyBuf(uint32_t idx, uint32_t cap)
    {
        uint32_t addr = low.alloc(cap);
        runtime().slot(idx).bodyPtr = addr;
        return low.raw(addr);
    }

    void setLen(uint32_t idx, uint32_t argCount, uint32_t bodyBytes, bool savesLR)
    {
        runtime().slot(idx).setStaticInfo(argCount, bodyBytes, savesLR);
    }

    // No body: a callee only ever CALL'd within this test, never itself
    // translated — argCount is the one fact that read reaches. With a
    // body: an ordinary Instr[] fixture, encoded fresh into cap bytes of
    // low memory and registered whole (this is the procedure translateProc
    // itself is asked to compile).
    void set(uint32_t idx, uint32_t argCount, bool savesLR, const Instr *body = nullptr, uint32_t count = 0, uint32_t cap = 64)
    {
        if(body == nullptr)
        {
            setLen(idx, argCount, /*bodyBytes=*/0, savesLR);
            return;
        }
        uint8_t *raw = bodyBuf(idx, cap);
        uint32_t len = encodeBody(body, count, raw, cap);
        setLen(idx, argCount, len, savesLR);
    }

    const uint16_t *code() const { return low.code(arenaBase); }
};

static uint32_t literalSiteCount(const uint16_t *buf, uint32_t halfwords)
{
    uint32_t n = 0;
    for(uint32_t i = 0; i < halfwords; i++)
    {
        uint16_t off;
        if(ArmV6M::getLiteralOffset(buf[i], off))
        {
            n++;
        }
    }
    return n;
}

/** The 32-bit word the literal load at halfword index site actually
 *  reaches, resolved exactly the way the hardware does — Align(pc,4) with
 *  pc being the site's own address + 4. Returns false if it lands outside
 *  the emitted output, which is the failure this indirection exists to
 *  catch. */
static bool loadedWord(const uint16_t *buf, uint32_t halfwords, uint32_t site, uint32_t &valueOut)
{
    uint16_t off;
    if(!ArmV6M::getLiteralOffset(buf[site], off))
    {
        return false; // GCOV_EXCL_LINE — only on a failing test's own bad site
    }
    uint32_t target = ((site * 2 + 4) & ~3u) + off * 4u;
    if(target % 4 != 0 || target / 2 + 1 >= halfwords)
    {
        return false; // GCOV_EXCL_LINE — the failure this helper exists to report
    }
    valueOut = (uint32_t)buf[target / 2] | ((uint32_t)buf[target / 2 + 1] << 16);
    return true;
}

/** Index of the nth (0-based) literal load in the output, or halfwords if
 *  there aren't that many. */
static uint32_t nthLiteralSite(const uint16_t *buf, uint32_t halfwords, uint32_t n)
{
    for(uint32_t i = 0; i < halfwords; i++)
    {
        uint16_t off;
        if(ArmV6M::getLiteralOffset(buf[i], off) && n-- == 0)
        {
            return i;
        }
    }
    return halfwords; // GCOV_EXCL_LINE — only when a test asks for a site that isn't there
}

TEST(TranslateProc0EntryProcedure)
{
    // proc0 makes a CALL, so it's non-leaf (savesLR): the prologue gains
    // push{lr}, and RETURN dispatches through returnHelperFromStack
    // (index 2, offset 8). argCount=0 keeps initialSpilledCount at 0 —
    // the ordinary non-leaf case, not the inline-pop-and-reclaim one.
    // The call record is now force-pooled (abi_strategy.cpp's
    // abiEmitCall) — one placeholder halfword at the call site, the real
    // packed value landing in the pool word this procedure's own
    // end-of-procedure flush emits.
    FakeRuntime<2> rt;
    rt.set(0, 0, /*savesLR=*/true, kProc0Body, 3);
    rt.set(1, 1, /*savesLR=*/false);
    uint32_t halfwordCount = translateProc(/*procIdx=*/0, rt.runtime(), LRU_TICK);

    CHECK(halfwordCount == 14);

    const uint16_t expected[] = {
        PROLOGUE_STUB,
        ArmV6M::pushWithLr(ArmV6M::LoRegs{0}),           // PUSH {lr}  (savesLR — proc0 makes a CALL)
        ArmV6M::movs(ArmV6M::LoReg(0), ArmV6M::Imm<8>(37)), // MOVS r0, #37  (CONST 37, stays pending until CALL flushes it)
        ArmV6M::ldrPc(ArmV6M::LoReg(1), ArmV6M::Uoff<2, 8>(12)), // LDR r1,[pc,#12] — the call record, force-pooled
        ArmV6M::movs(ArmV6M::LoReg(2), ArmV6M::Imm<8>(1)), // MOVS r2, #1  (calleeIndex=1, fits imm8)
        ArmV6M::mov(ArmV6M::AnyReg(3), ArmV6M::AnyReg(10)), ArmV6M::ldr(ArmV6M::LoReg(3), ArmV6M::LoReg(3), ArmV6M::Uoff<2, 5>(0)),
        ArmV6M::bx(ArmV6M::AnyReg(3)),                     // MOV r3,r10; LDR r3,[r3,#0]; BX r3  (callHelper)
        RETURN_VIA_STACK,
        0x0000, 0x000F,                                    // pool word: packRecord(procIdx=0, k+1=15)
    };
    for(uint32_t i = 0; i < halfwordCount; i++)
    {
        CHECK(rt.code()[i] == expected[i]);
    }
}

TEST(TranslateProc1Callee)
{
    // The callee prologue always flushes the incoming last argument into
    // its home register (physReg(0)=r7) unconditionally now, regardless
    // of how the body goes on to use it. LOAD(0) is then just an ordinary
    // in-window read (a deferred producer, itself free), folding straight
    // into the following opImm(ADD,5). No CALL here, so entirely
    // unaffected by the call record's own pooling change.
    FakeRuntime<2> rt;
    rt.set(1, 1, /*savesLR=*/false, kProc1Body, 3);
    uint32_t halfwordCount = translateProc(/*procIdx=*/1, rt.runtime(), LRU_TICK);

    CHECK(halfwordCount == 7);

    const uint16_t expected[] = {
        PROLOGUE_STUB,
        ArmV6M::mov(ArmV6M::AnyReg(7), ArmV6M::AnyReg(0)),                 // MOV r7, r0  (callee prologue: incoming last arg always flushed into physReg(0))
        ArmV6M::adds(ArmV6M::LoReg(0), ArmV6M::LoReg(7), ArmV6M::Imm<3>(5)), // ADDS r0, r7, #5  (LOAD(0)+opImm(ADD,5), folded straight into acc)
        RETURN_VIA_LR,
    };
    for(uint32_t i = 0; i < halfwordCount; i++)
    {
        CHECK(rt.code()[i] == expected[i]);
    }
}

TEST(OverflowIsReportedRatherThanOverrunningTheBuffer)
{
    FakeRuntime<2> rt(/*arenaBytes=*/8); // too small for the prologue and a single body instruction
    rt.set(0, 0, /*savesLR=*/true, kProc0Body, 3);
    rt.set(1, 1, /*savesLR=*/false);
    EXPECT_RESOURCE_ERROR(RESOURCE_EXHAUSTED_ARENA, translateProc(0, rt.runtime(), LRU_TICK));
}

TEST(CallToAProcedureIndexTheProgramDoesntHaveIsReported)
{
    // calleeIndex comes off the wire and Runtime::slot() applies no bound
    // of its own, so this is the one program-shape rejection the
    // translator itself has to make. Reported as a PROGRAM code, not an
    // EXHAUSTED one: no arena size makes a call to procedure 5 of a
    // one-procedure program work.
    const Instr body[] = {CONST(1), call(5), bare(Op::RETURN)};
    FakeRuntime<1> rt(/*arenaBytes=*/128);
    rt.set(0, 0, /*savesLR=*/true, body, 3);
    EXPECT_RESOURCE_ERROR(RESOURCE_PROGRAM_CALLEE_RANGE, translateProc(0, rt.runtime(), LRU_TICK));
}

TEST(LoopBackEdgeBailsWhenTheBodyExceedsTheEncodableBranchRange)
{
    // A back-edge past Ioff<1,11>'s +-2048-byte reach must fail, not wrap
    // into a silently retargeted branch.
    Instr body[404];
    uint32_t n = 0;
    body[n++] = bare(Op::LOOP);
    body[n++] = CONST(1);
    body[n++] = bare(Op::BLOCK_END);
    for(uint32_t i = 0; i < 400; i++)
    {
        body[n++] = bare(Op::CLZ);
    }
    body[n++] = bare(Op::BLOCK_END);
    body[n++] = bare(Op::RETURN);

    FakeRuntime<1> rt(/*arenaBytes=*/8192); // generous -- the failure under test must come from
                                            // the branch-range check, not emit()'s own capacity check
    rt.set(0, 1, /*savesLR=*/false, body, n, /*cap=*/2048);
    EXPECT_RESOURCE_ERROR(RESOURCE_LIMIT_LOOP_BACK_EDGE, translateProc(0, rt.runtime(), LRU_TICK));
}

TEST(SpillLoadBailsWhenTheOffsetExceedsTheEncodableRange)
{
    // Same defect class as F5/F6, found while fixing them: Window::
    // spillOffset's callers built Uoff<2,8> from an unbounded offset with
    // no range check, corrupting the destination-register field of the
    // LDR instead of just truncating the offset.
    const Instr body[] = {LOAD(0), bare(Op::RETURN)};
    FakeRuntime<1> rt;
    // tos=261 (argCount, all out-of-window) -> spillOffset(0) ==
    // 4*(spilledCount(261)-1) == 4*256 == 1024, past Uoff<2,8>::maxValue (1020).
    rt.set(0, 261, /*savesLR=*/false, body, sizeof(body) / sizeof(body[0]));
    EXPECT_RESOURCE_ERROR(RESOURCE_LIMIT_SPILL_OFFSET, translateProc(0, rt.runtime(), LRU_TICK));
}

// The tests below exercise LOOP/BR_TABLE/comparisons-as-values/unary ops/
// the last-argument's home slot/block-nesting-overflow through
// translateProc()'s main loop. End-to-end behavioral correctness for all
// of these is
// already proven on real QEMU (test/qemu/test_*.cpp), but that binary
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
        // isa-core.md §8.7: the loop-exit edge starts acc poisoned, so
        // RETURN needs its own fresh producer rather than reading whatever
        // the body last left behind.
        LOAD(0),
        bare(Op::RETURN),
    };
    FakeRuntime<1> rt;
    rt.set(0, 0, /*savesLR=*/false, body, sizeof(body) / sizeof(body[0]));
    uint32_t halfwordCount = translateProc(0, rt.runtime(), LRU_TICK);
    CHECK(halfwordCount == 13);

    const uint16_t expected[] = {
        PROLOGUE_STUB,
        ArmV6M::movs(ArmV6M::LoReg(7), ArmV6M::Imm<8>(3)), // MOVS r7,#3 (CONST 3, folds straight into PUSH's dest r7)
        ArmV6M::mov(ArmV6M::AnyReg(0), ArmV6M::AnyReg(7)), // MOV r0,r7 (openLoop's own flushLive — loopStart begins right here)
        ArmV6M::cmp(ArmV6M::LoReg(7), ArmV6M::Imm<8>(0)),  // CMP r7,#0 (testAccNonzero reads the cond block's LOAD(0) directly from r7, no flush needed)
        ArmV6M::beq(ArmV6M::Ioff<1, 8>(4)),                // BEQ +4 — exit branch (inverse of NE), skips the loop when acc==0
        ArmV6M::subs(ArmV6M::LoReg(7), ArmV6M::LoReg(7), ArmV6M::Imm<3>(1)), // SUBS r7,r7,#1 (body's LOAD(0)+SUB(1)+STORE(0), all folded in place)
        ArmV6M::mov(ArmV6M::AnyReg(0), ArmV6M::AnyReg(7)), // MOV r0,r7 (LoopBody close's own flushLive, right before the back-edge)
        ArmV6M::b(ArmV6M::Ioff<1, 11>(-12)),               // B -12 — unconditional back-edge, to loopStart (openLoop's own flushLive above)
        ArmV6M::mov(ArmV6M::AnyReg(0), ArmV6M::AnyReg(7)), // MOV r0,r7 (RETURN's flush of the post-loop LOAD(0) — the exit edge's own fresh producer)
        RETURN_VIA_LR,
    };
    for(uint32_t i = 0; i < halfwordCount; i++)
    {
        CHECK(rt.code()[i] == expected[i]);
    }
}

TEST(BrTableJumpTableHelperViaFullPipeline)
{
    // N >= 2 reaches openBrTableJump's own MOV/LDR/BLX-through-helper-
    // vector call sequence into brTableJumpHelper (jit-armv6m/runtime/
    // runtime.S) — not reachable through the two-block path this file's
    // other tests exercise.
    const Instr body[] = {
        CONST(1), brTable(3),
            CONST(10), bare(Op::BLOCK_END),
            CONST(20), bare(Op::BLOCK_END),
            CONST(30), bare(Op::BLOCK_END),
            CONST(40), bare(Op::BLOCK_END),
        // isa-core.md §8.7: only case[3] leaves acc live at the merge and
        // the others do too, but nothing here reads it — RETURN gets its
        // own fresh producer either way.
        CONST(0),
        bare(Op::RETURN),
    };
    FakeRuntime<1> rt(/*arenaBytes=*/128);
    rt.set(0, 0, /*savesLR=*/true, body, sizeof(body) / sizeof(body[0]));
    uint32_t halfwordCount = translateProc(0, rt.runtime(), LRU_TICK);
    CHECK(halfwordCount == 23);

    const uint16_t expected[] = {
        PROLOGUE_STUB,
        ArmV6M::pushWithLr(ArmV6M::LoRegs{0}),              // PUSH {lr}  (savesLR — BR_TABLE N>=2 clobbers lr via BLX)
        ArmV6M::movs(ArmV6M::LoReg(0), ArmV6M::Imm<8>(1)),  // MOVS r0,#1 (CONST 1, the jump-table selector)
        ArmV6M::movs(ArmV6M::LoReg(2), ArmV6M::Imm<8>(3)),  // MOVS r2,#3 (n=3, into SCRATCH_REG)
        ArmV6M::mov(ArmV6M::AnyReg(3), ArmV6M::AnyReg(10)), // MOV r3,r10 (HELPER_VEC_REG)
        ArmV6M::ldr(ArmV6M::LoReg(3), ArmV6M::LoReg(3), ArmV6M::Uoff<2, 5>(20)), // LDR r3,[r3,#20] — brTableJumpHelper slot
        ArmV6M::blx(ArmV6M::AnyReg(3)),                     // BLX r3 (brTableJumpHelper — lr now points at the table right after)
        0x0008, // -- jump table slot 0 (case0): 8 bytes past the table base
        0x000c, // -- jump table slot 1 (case1)
        0x0010, // -- jump table slot 2 (case2)
        0x0014, // -- jump table slot 3 (case3, the default case — a block of its own now)
        ArmV6M::movs(ArmV6M::LoReg(0), ArmV6M::Imm<8>(10)), // MOVS r0,#10 (case0: CONST 10)
        ArmV6M::b(ArmV6M::Ioff<1, 11>(8)),                  // B +8 — skip to the merge
        ArmV6M::movs(ArmV6M::LoReg(0), ArmV6M::Imm<8>(20)), // MOVS r0,#20 (case1: CONST 20)
        ArmV6M::b(ArmV6M::Ioff<1, 11>(4)),                  // B +4 — skip to the merge
        ArmV6M::movs(ArmV6M::LoReg(0), ArmV6M::Imm<8>(30)), // MOVS r0,#30 (case2: CONST 30)
        ArmV6M::b(ArmV6M::Ioff<1, 11>(0)),                  // B +0 — skip to the merge
        ArmV6M::movs(ArmV6M::LoReg(0), ArmV6M::Imm<8>(40)), // MOVS r0,#40 (case3: the default, falls straight into the merge)
        ArmV6M::movs(ArmV6M::LoReg(0), ArmV6M::Imm<8>(0)),  // MOVS r0,#0  (RETURN's own fresh producer)
        RETURN_VIA_STACK,
    };
    for(uint32_t i = 0; i < halfwordCount; i++)
    {
        CHECK(rt.code()[i] == expected[i]);
    }
}

TEST(ComparisonFusesIntoBrTableGuard)
{
    const Instr body[] = {
        CONST(5), opImm(Op::GT_U, 3), brTable(1),
            CONST(1), bare(Op::BLOCK_END),
            CONST(2), bare(Op::BLOCK_END),
        // isa-core.md §8.7: the if/else merge point starts acc poisoned
        // regardless of which arm ran — RETURN needs its own fresh
        // producer.
        CONST(0),
        bare(Op::RETURN),
    };
    FakeRuntime<1> rt;
    rt.set(0, 0, /*savesLR=*/false, body, sizeof(body) / sizeof(body[0]));
    uint32_t halfwordCount = translateProc(0, rt.runtime(), LRU_TICK);
    CHECK(halfwordCount == 12);

    const uint16_t expected[] = {
        PROLOGUE_STUB,
        ArmV6M::movs(ArmV6M::LoReg(0), ArmV6M::Imm<8>(5)), // MOVS r0,#5 (CONST 5)
        ArmV6M::cmp(ArmV6M::LoReg(0), ArmV6M::Imm<8>(3)),  // CMP r0,#3 (GT_U operand, fused straight into the BR_TABLE guard)
        ArmV6M::bhi(ArmV6M::Ioff<1, 8>(2)),                // BHI +2 — GT_U's own true condition: jump straight to case1
        ArmV6M::movs(ArmV6M::LoReg(0), ArmV6M::Imm<8>(1)), // MOVS r0,#1 (case0: CONST 1)
        ArmV6M::b(ArmV6M::Ioff<1, 11>(0)),                 // B +0 — skip case1, to RETURN
        ArmV6M::movs(ArmV6M::LoReg(0), ArmV6M::Imm<8>(2)), // MOVS r0,#2 (case1: CONST 2)
        ArmV6M::movs(ArmV6M::LoReg(0), ArmV6M::Imm<8>(0)), // MOVS r0,#0 (RETURN's flush of the post-construct CONST(0) — the merge point's own fresh producer)
        RETURN_VIA_LR,
    };
    for(uint32_t i = 0; i < halfwordCount; i++)
    {
        CHECK(rt.code()[i] == expected[i]);
    }
}

TEST(ComparisonMaterializesAsOrdinaryValue)
{
    // No BR_TABLE/LOOP-exit right after it — takes the
    // materializeComparison path, not fusion.
    const Instr body[] = {CONST(5), opImm(Op::GT_U, 3), bare(Op::RETURN)};
    FakeRuntime<1> rt;
    rt.set(0, 0, /*savesLR=*/false, body, sizeof(body) / sizeof(body[0]));
    uint32_t halfwordCount = translateProc(0, rt.runtime(), LRU_TICK);
    CHECK(halfwordCount == 11);

    const uint16_t expected[] = {
        PROLOGUE_STUB,
        ArmV6M::movs(ArmV6M::LoReg(0), ArmV6M::Imm<8>(5)), // MOVS r0,#5 (CONST 5)
        ArmV6M::cmp(ArmV6M::LoReg(0), ArmV6M::Imm<8>(3)),  // CMP r0,#3 (GT_U operand)
        ArmV6M::bls(ArmV6M::Ioff<1, 8>(2)),                // BLS +2 — inverse of GT_U(HI): not-taken skips straight to the r0=0 case
        ArmV6M::movs(ArmV6M::LoReg(0), ArmV6M::Imm<8>(1)), // MOVS r0,#1 (GT_U true)
        ArmV6M::b(ArmV6M::Ioff<1, 11>(0)),                 // B +0 — skip over the r0=0 case
        ArmV6M::movs(ArmV6M::LoReg(0), ArmV6M::Imm<8>(0)), // MOVS r0,#0 (GT_U false)
        RETURN_VIA_LR,
    };
    for(uint32_t i = 0; i < halfwordCount; i++)
    {
        CHECK(rt.code()[i] == expected[i]);
    }
}

TEST(NegViaFullPipeline)
{
    const Instr body[] = {CONST(5), bare(Op::NEG), bare(Op::RETURN)};
    FakeRuntime<1> rt;
    rt.set(0, 0, /*savesLR=*/false, body, sizeof(body) / sizeof(body[0]));
    uint32_t halfwordCount = translateProc(0, rt.runtime(), LRU_TICK);
    CHECK(halfwordCount == 7);

    const uint16_t expected[] = {
        PROLOGUE_STUB,
        ArmV6M::movs(ArmV6M::LoReg(0), ArmV6M::Imm<8>(5)), // MOVS r0,#5 (CONST 5)
        ArmV6M::negs(ArmV6M::LoReg(0), ArmV6M::LoReg(0)),  // NEGS r0,r0 (NEG)
        RETURN_VIA_LR,
    };
    for(uint32_t i = 0; i < halfwordCount; i++)
    {
        CHECK(rt.code()[i] == expected[i]);
    }
}

TEST(ClzHelperViaFullPipeline)
{
    // Reaches emitUnary's own MOV/LDR/BLX-through-helper-vector sequence
    // into clzHelper (runtime/runtime.S) — that sequence's shape is
    // unit-tested directly in test_unaryops.cpp; this is the
    // caller-side wiring around it.
    const Instr body[] = {CONST(5), bare(Op::CLZ), bare(Op::RETURN)};
    FakeRuntime<1> rt;
    rt.set(0, 0, /*savesLR=*/true, body, sizeof(body) / sizeof(body[0]));
    uint32_t halfwordCount = translateProc(0, rt.runtime(), LRU_TICK);
    CHECK(halfwordCount == 10);

    const uint16_t expected[] = {
        PROLOGUE_STUB,
        ArmV6M::pushWithLr(ArmV6M::LoRegs{0}),             // PUSH {lr}  (savesLR — CLZ dispatches through the helper vector, clobbers real lr)
        ArmV6M::movs(ArmV6M::LoReg(0), ArmV6M::Imm<8>(5)), // MOVS r0, #5  (CONST 5)
        // CLZ helper dispatch (clzHelper, helper-vector index 3, offset 12) — BLX not BX, since clzHelper returns via `bx lr` instead of tail-jumping
        ArmV6M::mov(ArmV6M::AnyReg(3), ArmV6M::AnyReg(10)), ArmV6M::ldr(ArmV6M::LoReg(3), ArmV6M::LoReg(3), ArmV6M::Uoff<2, 5>(12)),
        ArmV6M::blx(ArmV6M::AnyReg(3)),
        RETURN_VIA_STACK,
    };
    for(uint32_t i = 0; i < halfwordCount; i++)
    {
        CHECK(rt.code()[i] == expected[i]);
    }
}

TEST(LastArgumentHomeSlotReadTwiceViaLoad)
{
    // The callee prologue always flushes the last argument into
    // physReg(argCount-1) unconditionally, so two ordinary LOADs of that
    // same in-window slot both just read the already-correct value.
    const Instr body[] = {LOAD(0), LOAD(0), opReg(Op::ADD, 0), bare(Op::RETURN)};
    FakeRuntime<1> rt;
    rt.set(0, 1, /*savesLR=*/false, body, sizeof(body) / sizeof(body[0]));
    uint32_t halfwordCount = translateProc(0, rt.runtime(), LRU_TICK);
    CHECK(halfwordCount == 7);

    const uint16_t expected[] = {
        PROLOGUE_STUB,
        ArmV6M::mov(ArmV6M::AnyReg(7), ArmV6M::AnyReg(0)),                  // MOV r7, r0  (callee prologue: incoming last arg flushed into physReg(0))
        ArmV6M::adds(ArmV6M::LoReg(0), ArmV6M::LoReg(7), ArmV6M::LoReg(7)), // ADDS r0, r7, r7  (both LOAD(0)s fold to reading r7; REG_ACC ADD combines them)
        RETURN_VIA_LR,
    };
    for(uint32_t i = 0; i < halfwordCount; i++)
    {
        CHECK(rt.code()[i] == expected[i]);
    }
}

TEST(LastArgumentHomeSlotReadViaPopAcc)
{
    // At tos == argCount, window.topReg() *is* physReg(lastArgSlot). The
    // callee prologue flushes it unconditionally, so POP_ACC's implicit
    // window-top read is correct by construction. POP_ACC rather than
    // PEEK_PEEK keeps the program accLive-valid into RETURN (isa-core.md
    // §10.1: PEEK_PEEK writes back in place and clobbers acc).
    const Instr body[] = {opStack(Op::ADD, Combo::POP_ACC), bare(Op::RETURN)};
    FakeRuntime<1> rt;
    rt.set(0, 1, /*savesLR=*/false, body, sizeof(body) / sizeof(body[0]));
    uint32_t halfwordCount = translateProc(0, rt.runtime(), LRU_TICK);
    CHECK(halfwordCount == 7);

    const uint16_t expected[] = {
        PROLOGUE_STUB,
        ArmV6M::mov(ArmV6M::AnyReg(7), ArmV6M::AnyReg(0)),                  // MOV r7, r0  (callee prologue: incoming last arg flushed into physReg(lastArgSlot))
        ArmV6M::adds(ArmV6M::LoReg(0), ArmV6M::LoReg(7), ArmV6M::LoReg(7)), // ADDS r0, r7, r7  (POP_ACC ADD: acc and the popped home slot are now the same, correctly-flushed value)
        RETURN_VIA_LR,
    };
    for(uint32_t i = 0; i < halfwordCount; i++)
    {
        CHECK(rt.code()[i] == expected[i]);
    }
}

TEST(CaseClosesViaTerminatorThroughFullPipeline)
{
    // A non-last case closing via bare RETURN, dispatched through
    // translateProc's own closeFrameForTerminator — blocks.h's
    // closeCaseViaTerminator is unit-tested directly (test_blocks.cpp),
    // this is the caller-side switch dispatch around it.
    const Instr body[] = {
        CONST(5), opImm(Op::GT_U, 3), brTable(1),
            CONST(1), bare(Op::RETURN),
            CONST(2), bare(Op::BLOCK_END),
        // isa-core.md §8.7: the if/else merge point starts acc poisoned
        // regardless of which arm ran — the trailing RETURN needs its own
        // fresh producer.
        CONST(0),
        bare(Op::RETURN),
    };
    FakeRuntime<1> rt;
    rt.set(0, 0, /*savesLR=*/false, body, sizeof(body) / sizeof(body[0]));
    uint32_t halfwordCount = translateProc(0, rt.runtime(), LRU_TICK);
    CHECK(halfwordCount == 14);

    const uint16_t expected[] = {
        PROLOGUE_STUB,
        ArmV6M::movs(ArmV6M::LoReg(0), ArmV6M::Imm<8>(5)), // MOVS r0, #5  (CONST 5)
        ArmV6M::cmp(ArmV6M::LoReg(0), ArmV6M::Imm<8>(3)),  // CMP r0, #3   (GT_U fused into the brTable guard)
        ArmV6M::bhi(ArmV6M::Ioff<1, 8>(6)),                // BHI -> case1  (guard false: skip case0 entirely)
        ArmV6M::movs(ArmV6M::LoReg(0), ArmV6M::Imm<8>(1)), // MOVS r0, #1  (case0: CONST 1)
        RETURN_VIA_LR,                                     // case0's own bare RETURN — closeFrameForTerminator's Case dispatch
        ArmV6M::movs(ArmV6M::LoReg(0), ArmV6M::Imm<8>(2)), // MOVS r0, #2  (case1: CONST 2)
        ArmV6M::movs(ArmV6M::LoReg(0), ArmV6M::Imm<8>(0)), // MOVS r0, #0  (trailing RETURN's flush of the post-construct CONST(0) — the merge point's own fresh producer)
        RETURN_VIA_LR,                                     // the trailing bare RETURN after case1's BLOCK_END
    };
    for(uint32_t i = 0; i < halfwordCount; i++)
    {
        CHECK(rt.code()[i] == expected[i]);
    }
}

// The tests below cover the remaining paths in translate_proc.cpp's main
// dispatch switch: TRAP (both at the top level and inside an open
// block), out-of-window LOAD/STORE, a STORE not preceded by a foldable
// producer, CONST's large-immediate/fold-into-STORE paths, REG_REG
// (opRegWriteback) both in- and out-of-window, POP_ACC/PEEK_PEEK stack
// combos, a LOOP body that closes via a bare RETURN instead of BLOCK_END,
// and REVBITS's helper-vector call (mirrored after ClzHelperViaFullPipeline
// above) — the ordinary, "not the fused/aligned case" side of paths this
// file's other tests already cover the fused side of.

TEST(TrapAtTopLevel)
{
    const Instr body[] = {trapInstr(3)};
    FakeRuntime<1> rt;
    rt.set(0, 0, /*savesLR=*/false, body, 1);
    uint32_t halfwordCount = translateProc(0, rt.runtime(), LRU_TICK);
    // The code goes into ACC_REG plainly — no high-bit sentinel to widen
    // it past MOVS's own imm8, so nothing pools and no window teardown
    // precedes the dispatch either (trapHelper restores savedSp instead).
    CHECK(halfwordCount == 6);

    const uint16_t expected[] = {
        PROLOGUE_STUB,
        ArmV6M::movs(ArmV6M::LoReg(0), ArmV6M::Imm<8>(3)), // MOVS r0, #3 — the trap code itself
        TRAP_VIA_HELPER,
    };
    for(uint32_t i = 0; i < halfwordCount; i++)
    {
        CHECK(rt.code()[i] == expected[i]);
    }
}

TEST(TrapInsideCaseClosesItAndContinuesToNextCase)
{
    // Same shape as CaseClosesViaTerminatorThroughFullPipeline above, but
    // with TRAP as the terminator instead of RETURN — closeFrameForTerminator's
    // "frame != nullptr" guard around TRAP is a distinct source line from
    // RETURN's identically-shaped guard. Nothing pools here: trapInstr(9)'s
    // code is emitted plainly, so it fits MOVS's own imm8 and case0's
    // terminator close has no pending pool entry to flush (which is why
    // there is no alignment pad between case0 and case1 either).
    const Instr body[] = {
        CONST(5), opImm(Op::GT_U, 3), brTable(1),
            CONST(1), trapInstr(9),
            CONST(2), bare(Op::BLOCK_END),
        // isa-core.md §8.7: the if/else merge point starts acc poisoned
        // regardless of which arm ran — the trailing RETURN needs its own
        // fresh producer.
        CONST(0),
        bare(Op::RETURN),
    };
    FakeRuntime<1> rt;
    rt.set(0, 0, /*savesLR=*/false, body, sizeof(body) / sizeof(body[0]));
    uint32_t halfwordCount = translateProc(0, rt.runtime(), LRU_TICK);
    CHECK(halfwordCount == 14);

    const uint16_t expected[] = {
        PROLOGUE_STUB,
        ArmV6M::movs(ArmV6M::LoReg(0), ArmV6M::Imm<8>(5)), // MOVS r0, #5  (CONST 5)
        ArmV6M::cmp(ArmV6M::LoReg(0), ArmV6M::Imm<8>(3)),  // CMP r0, #3  (opImm(GT_U,3), fused straight into the brTable guard)
        ArmV6M::bhi(ArmV6M::Ioff<1, 8>(6)),                // BHI -> case1's CONST(2), skipping case0's TRAP
        ArmV6M::movs(ArmV6M::LoReg(0), ArmV6M::Imm<8>(9)), // MOVS r0, #9  (TRAP's own code)
        TRAP_VIA_HELPER, // *not* RETURN_VIA_LR: a nested TRAP unwinds rather than returning to its caller
        ArmV6M::movs(ArmV6M::LoReg(0), ArmV6M::Imm<8>(2)), // MOVS r0, #2  (case1's CONST 2)
        ArmV6M::movs(ArmV6M::LoReg(0), ArmV6M::Imm<8>(0)), // MOVS r0, #0  (trailing RETURN's flush of the post-construct CONST(0) — the merge point's own fresh producer)
        RETURN_VIA_LR,
    };
    for(uint32_t i = 0; i < halfwordCount; i++)
    {
        CHECK(rt.code()[i] == expected[i]);
    }
}

TEST(LoadFromOutOfWindowSlot)
{
    // argCount=5 > WINDOW_SIZE(4) — slot 0 is spilled from procedure
    // entry, so LOAD(0) must reload it via ldrSp instead of reading a
    // window register directly. The callee prologue's own unconditional
    // flush targets slot 4 (physReg(4) wraps back to r7), unrelated to
    // slot 0's own spill/reload.
    const Instr body[] = {LOAD(0), bare(Op::RETURN)};
    FakeRuntime<1> rt;
    rt.set(0, 5, /*savesLR=*/false, body, 2);
    uint32_t halfwordCount = translateProc(0, rt.runtime(), LRU_TICK);
    CHECK(halfwordCount == 8);

    const uint16_t expected[] = {
        PROLOGUE_STUB,
        ArmV6M::mov(ArmV6M::AnyReg(7), ArmV6M::AnyReg(0)),      // MOV r7, r0  (callee prologue: incoming last arg flushed into physReg(4)=r7)
        ArmV6M::ldrSp(ArmV6M::LoReg(0), ArmV6M::Uoff<2, 8>(0)), // LDR r0,[sp,#0]  (LOAD(0): slot0 spilled, argCount=5>WINDOW_SIZE)
        ArmV6M::incrSp(ArmV6M::Uoff<2, 7>(4)),                  // ADD sp,#4  (discardWindow: reclaim the one spilled arg slot before returning)
        RETURN_VIA_LR,
    };
    for(uint32_t i = 0; i < halfwordCount; i++)
    {
        CHECK(rt.code()[i] == expected[i]);
    }
}

TEST(StoreStandaloneInWindowWhenNotPrecededByAFoldableProducer)
{
    // PUSH doesn't peek-fold a following STORE (only CONST/LOAD/unary/
    // arithmetic do) — this is the switch's own bare `case Op::STORE`
    // dispatch, not the far-more-common fold path this file's other
    // tests exercise via peekStoreFold.
    const Instr body[] = {CONST(9), PUSH(), STORE(0), bare(Op::RETURN)};
    FakeRuntime<1> rt;
    rt.set(0, 1, /*savesLR=*/false, body, sizeof(body) / sizeof(body[0]));
    uint32_t halfwordCount = translateProc(0, rt.runtime(), LRU_TICK);
    CHECK(halfwordCount == 9);

    const uint16_t expected[] = {
        PROLOGUE_STUB,
        ArmV6M::mov(ArmV6M::AnyReg(7), ArmV6M::AnyReg(0)), // MOV r7, r0  (callee prologue: incoming last arg flushed into physReg(0))
        ArmV6M::movs(ArmV6M::LoReg(6), ArmV6M::Imm<8>(9)), // MOVS r6, #9  (CONST 9, into physReg(1) for the PUSH)
        ArmV6M::mov(ArmV6M::AnyReg(7), ArmV6M::AnyReg(6)), // MOV r7, r6  (STORE(0): standalone, not preceded by a foldable producer)
        ArmV6M::mov(ArmV6M::AnyReg(0), ArmV6M::AnyReg(7)), // MOV r0, r7  (RETURN's own acc flush, resyncing from the STORE's target)
        RETURN_VIA_LR,
    };
    for(uint32_t i = 0; i < halfwordCount; i++)
    {
        CHECK(rt.code()[i] == expected[i]);
    }
}

TEST(StoreStandaloneOutOfWindowSlot)
{
    // peekStoreFold never folds an out-of-window STORE target regardless
    // of what precedes it (fold.reg is a physical register, meaningless
    // for a spilled slot), so NEG's own STORE(0) here always takes the
    // standalone out-of-window path. argCount=5's own last argument
    // (slot 4, physReg(4)=r7) is unrelated to slot 0 and gets the callee
    // prologue's usual unconditional flush regardless of never being
    // referenced by this body. NEG reads that value directly from r7
    // (negs's independent source field), and the STORE reads NEG's
    // result directly from ACC_REG — no round-trip through ACC_REG for
    // either.
    const Instr body[] = {bare(Op::NEG), STORE(0), bare(Op::RETURN)};
    FakeRuntime<1> rt;
    rt.set(0, 5, /*savesLR=*/false, body, 3);
    uint32_t halfwordCount = translateProc(0, rt.runtime(), LRU_TICK);
    CHECK(halfwordCount == 9);

    const uint16_t expected[] = {
        PROLOGUE_STUB,
        ArmV6M::mov(ArmV6M::AnyReg(7), ArmV6M::AnyReg(0)),      // MOV r7, r0  (callee prologue: incoming last arg flushed into physReg(4)=r7, unreferenced by this body)
        ArmV6M::negs(ArmV6M::LoReg(0), ArmV6M::LoReg(7)),       // NEGS r0, r7  (NEG reads r7 directly, dest ACC_REG since the STORE that follows is out-of-window)
        ArmV6M::strSp(ArmV6M::LoReg(0), ArmV6M::Uoff<2, 8>(0)), // STR r0,[sp,#0]  (STORE(0): out-of-window, peekStoreFold never applies here; reads NEG's result directly)
        ArmV6M::incrSp(ArmV6M::Uoff<2, 7>(4)),                  // ADD sp,#4  (discardWindow: reclaim the one spilled arg slot before returning)
        RETURN_VIA_LR,
    };
    for(uint32_t i = 0; i < halfwordCount; i++)
    {
        CHECK(rt.code()[i] == expected[i]);
    }
}

TEST(ConstTooLargeForImm8SynthesizesInsteadOfStayingPending)
{
    // CONST(1000) doesn't fit imm8, but 1000 = 125 << 3 does fit
    // materializeImm32's shift-trick (MOVS + LSLS, 2 halfwords) — so this
    // also guards that nothing here may become a literal load.
    const Instr body[] = {CONST(1000), bare(Op::RETURN)};
    FakeRuntime<1> rt;
    rt.set(0, 0, /*savesLR=*/false, body, 2);
    uint32_t halfwordCount = translateProc(0, rt.runtime(), LRU_TICK);
    CHECK(halfwordCount == 7);
    CHECK(literalSiteCount(rt.code(), halfwordCount) == 0);

    const uint16_t expected[] = {
        PROLOGUE_STUB,
        ArmV6M::movs(ArmV6M::LoReg(0), ArmV6M::Imm<8>(125)),                // MOVS r0, #125  (CONST 1000's shift-trick synthesis: 125<<3)
        ArmV6M::lsls(ArmV6M::LoReg(0), ArmV6M::LoReg(0), ArmV6M::Imm<5>(3)), // LSLS r0, r0, #3
        RETURN_VIA_LR,
    };
    for(uint32_t i = 0; i < halfwordCount; i++)
    {
        CHECK(rt.code()[i] == expected[i]);
    }
}

TEST(ConstFoldsDirectlyIntoAFollowingStore)
{
    const Instr body[] = {CONST(5), STORE(0), bare(Op::RETURN)};
    FakeRuntime<1> rt;
    rt.set(0, 1, /*savesLR=*/false, body, 3);
    uint32_t halfwordCount = translateProc(0, rt.runtime(), LRU_TICK);
    CHECK(halfwordCount == 8);

    const uint16_t expected[] = {
        PROLOGUE_STUB,
        ArmV6M::mov(ArmV6M::AnyReg(7), ArmV6M::AnyReg(ACC_REG)), // MOV r7, r0  (callee prologue: argCount=1's incoming last arg always flushed into physReg(0)=r7)
        ArmV6M::movs(ArmV6M::LoReg(7), ArmV6M::Imm<8>(5)),       // MOVS r7, #5  (CONST 5 folds straight into STORE(0)'s target r7, overwriting the arg)
        ArmV6M::mov(ArmV6M::AnyReg(ACC_REG), ArmV6M::AnyReg(7)), // MOV r0, r7  (RETURN's flush: acc's clean value now lives in r7, brought back to r0)
        RETURN_VIA_LR,
    };
    for(uint32_t i = 0; i < halfwordCount; i++)
    {
        CHECK(rt.code()[i] == expected[i]);
    }
}

TEST(RegRegOutOfWindowWritesBackToStackAfterComputing)
{
    // REG_REG (opRegWriteback) with an out-of-window target: the operand
    // reload (shared with REG_ACC) and REG_REG's own SCRATCH_REG-as-dest
    // plus explicit str-back are both otherwise only exercised indirectly
    // through binops.cpp's own lower-level tests, never through this
    // file's real dispatch. argCount=5's callee prologue flush leaves
    // accState Clean at physReg(4)=r7 (a plain copy of r0, same value) —
    // since nothing else produces a fresh value before this REG_REG runs,
    // its acc operand reads r7, not r0.
    const Instr body[] = {opRegWriteback(Op::ADD, 0), CONST(1), bare(Op::RETURN)};
    FakeRuntime<1> rt;
    rt.set(0, 5, /*savesLR=*/false, body, 3);
    uint32_t halfwordCount = translateProc(0, rt.runtime(), LRU_TICK);
    CHECK(halfwordCount == 11);

    const uint16_t expected[] = {
        PROLOGUE_STUB,
        ArmV6M::mov(ArmV6M::AnyReg(7), ArmV6M::AnyReg(0)),                                          // MOV r7, r0  (callee prologue: incoming last arg flushed into physReg(4)=r7)
        ArmV6M::ldrSp(ArmV6M::LoReg(SCRATCH_REG), ArmV6M::Uoff<2, 8>(0)),                           // LDR r2,[sp,#0]  (reload the REG_REG operand — slot 0 is out-of-window since argCount=5>WINDOW_SIZE)
        ArmV6M::adds(ArmV6M::LoReg(SCRATCH_REG), ArmV6M::LoReg(7), ArmV6M::LoReg(SCRATCH_REG)),     // ADDS r2, r7, r2  (REG_REG ADD: acc — now Clean at r7 — plus reloaded operand(r2), dest is SCRATCH_REG since target is out-of-window)
        ArmV6M::strSp(ArmV6M::LoReg(SCRATCH_REG), ArmV6M::Uoff<2, 8>(0)),                           // STR r2,[sp,#0]  (write the result back to the spilled slot)
        ArmV6M::movs(ArmV6M::LoReg(ACC_REG), ArmV6M::Imm<8>(1)),                                    // MOVS r0, #1  (CONST(1), flushed for RETURN)
        ArmV6M::incrSp(ArmV6M::Uoff<2, 7>(4)),                                                      // ADD sp, #4  (discardWindow reclaims the one spilled arg slot; savesLR is false here)
        RETURN_VIA_LR,
    };
    for(uint32_t i = 0; i < halfwordCount; i++)
    {
        CHECK(rt.code()[i] == expected[i]);
    }
}

TEST(RegRegInWindowWritesBackToItsOwnRegister)
{
    const Instr body[] = {CONST(5), PUSH(), opRegWriteback(Op::ADD, 0), LOAD(0), bare(Op::RETURN)};
    FakeRuntime<1> rt;
    rt.set(0, 0, /*savesLR=*/false, body, sizeof(body) / sizeof(body[0]));
    uint32_t halfwordCount = translateProc(0, rt.runtime(), LRU_TICK);
    CHECK(halfwordCount == 8);

    const uint16_t expected[] = {
        PROLOGUE_STUB,
        ArmV6M::movs(ArmV6M::LoReg(7), ArmV6M::Imm<8>(5)),                 // MOVS r7, #5  (CONST 5, flushed into physReg(0)=r7 by PUSH)
        ArmV6M::adds(ArmV6M::LoReg(7), ArmV6M::LoReg(7), ArmV6M::LoReg(7)), // ADDS r7, r7, r7  (REG_REG ADD, in-window: dest==operand==acc's current home, r7)
        ArmV6M::mov(ArmV6M::AnyReg(ACC_REG), ArmV6M::AnyReg(7)),           // MOV r0, r7  (RETURN's flush: LOAD(0)'s pending producer(r7) brought into acc)
        RETURN_VIA_LR,
    };
    for(uint32_t i = 0; i < halfwordCount; i++)
    {
        CHECK(rt.code()[i] == expected[i]);
    }
}

TEST(PopAccStackComboThroughFullPipeline)
{
    const Instr body[] = {CONST(3), PUSH(), CONST(2), opStack(Op::ADD, Combo::POP_ACC), bare(Op::RETURN)};
    FakeRuntime<1> rt;
    rt.set(0, 0, /*savesLR=*/false, body, sizeof(body) / sizeof(body[0]));
    uint32_t halfwordCount = translateProc(0, rt.runtime(), LRU_TICK);
    CHECK(halfwordCount == 7);

    const uint16_t expected[] = {
        PROLOGUE_STUB,
        ArmV6M::movs(ArmV6M::LoReg(7), ArmV6M::Imm<8>(3)),                          // MOVS r7, #3  (CONST 3, flushed into physReg(0)=r7 by PUSH)
        ArmV6M::adds(ArmV6M::LoReg(ACC_REG), ArmV6M::LoReg(7), ArmV6M::Imm<3>(2)),  // ADDS r0, r7, #2  (POP_ACC ADD: pending CONST 2 as the constant LHS, popped r7 as RHS, dest=acc)
        RETURN_VIA_LR,
    };
    for(uint32_t i = 0; i < halfwordCount; i++)
    {
        CHECK(rt.code()[i] == expected[i]);
    }
}

TEST(PeekPeekStackComboThroughFullPipeline)
{
    // PEEK_PEEK's dest is the stack top itself (physReg(tos-1) = r7 here),
    // computed in place with no tos change — CONST(3) materializes
    // straight into r7 for the PUSH rather than acc, since accState.flush
    // targets the push's own destination register directly. LOAD(0) then
    // moves that live value from its window slot (r7) into acc (r0) —
    // nothing follows it to fold into, so RETURN's flush materializes it.
    const Instr body[] = {CONST(3), PUSH(), CONST(6), opStack(Op::AND, Combo::PEEK_PEEK), LOAD(0), bare(Op::RETURN)};
    FakeRuntime<1> rt;
    rt.set(0, 0, /*savesLR=*/false, body, sizeof(body) / sizeof(body[0]));
    uint32_t halfwordCount = translateProc(0, rt.runtime(), LRU_TICK);

    CHECK(halfwordCount == 9);

    const uint16_t expected[] = {
        PROLOGUE_STUB,
        ArmV6M::movs(ArmV6M::LoReg(7), ArmV6M::Imm<8>(3)), // MOVS r7, #3  (CONST 3, into physReg(0) for the PUSH)
        ArmV6M::movs(ArmV6M::LoReg(0), ArmV6M::Imm<8>(6)), // MOVS r0, #6  (CONST 6, into acc)
        ArmV6M::ands(ArmV6M::LoReg(7), ArmV6M::LoReg(0)),  // ANDS r7, r0  (PEEK_PEEK: r7 &= acc, in place)
        ArmV6M::mov(ArmV6M::AnyReg(0), ArmV6M::AnyReg(7)), // MOV r0, r7   (LOAD(0): window slot -> acc, at RETURN's flush)
        RETURN_VIA_LR,
    };
    for(uint32_t i = 0; i < halfwordCount; i++)
    {
        CHECK(rt.code()[i] == expected[i]);
    }
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
    //
    // The empty cond block also means openLoop's own flushLive(ACC_REG)
    // (translateLoop's join-point flush, needed so the back-edge and the
    // fall-through agree on where acc lives) is the only producer
    // testAccNonzero ever sees here — nothing runs between them to
    // change it — so testAccNonzero's own read-in-place fix finds acc
    // already Clean at ACC_REG and adds nothing further.
    const Instr body[] = {
        bare(Op::LOOP), bare(Op::BLOCK_END),
        CONST(42), bare(Op::RETURN),
        CONST(999), bare(Op::RETURN),
    };
    FakeRuntime<1> rt;
    // argCount=1 (not 0): the callee prologue's own unconditional flush
    // into physReg(0)=r7 below is only observable at all when there's an
    // actual last argument to flush.
    rt.set(0, 1, /*savesLR=*/false, body, sizeof(body) / sizeof(body[0]));
    uint32_t halfwordCount = translateProc(0, rt.runtime(), LRU_TICK);
    CHECK(halfwordCount == 16);

    const uint16_t expected[] = {
        PROLOGUE_STUB,
        ArmV6M::mov(ArmV6M::AnyReg(7), ArmV6M::AnyReg(0)),        // MOV r7, r0  (callee prologue: incoming last arg flushed into physReg(0)=r7, unreferenced by this body)
        ArmV6M::mov(ArmV6M::AnyReg(0), ArmV6M::AnyReg(7)),        // MOV r0, r7  (openLoop's own flushLive(ACC_REG) — the loop's join-point flush, not testAccNonzero's)
        ArmV6M::cmp(ArmV6M::LoReg(ACC_REG), ArmV6M::Imm<8>(0)),   // CMP r0, #0  (LOOP condition: no fused comparison, testAccNonzero's fallback)
        ArmV6M::beq(ArmV6M::Ioff<1, 8>(6)),                       // BEQ exitFixup  (loop-exit branch, taken when acc==0)
        ArmV6M::movs(ArmV6M::LoReg(ACC_REG), ArmV6M::Imm<8>(42)), // MOVS r0, #42  (RETURN's flush of CONST(42)'s pending value)
        RETURN_VIA_LR,
        ArmV6M::ldrPc(ArmV6M::LoReg(ACC_REG), ArmV6M::Uoff<2, 8>(4)), // LDR r0,[pc,#4]  (CONST(999) doesn't fit imm8 or the shift trick, so it pools)
        RETURN_VIA_LR,
        0x03e7, 0x0000,                                           // pool word: 999
    };
    for(uint32_t i = 0; i < halfwordCount; i++)
    {
        CHECK(rt.code()[i] == expected[i]);
    }
}

TEST(RevbitsHelperViaFullPipeline)
{
    // Mirrors ClzHelperViaFullPipeline above, for REVBITS's own helper
    // vector index (revbitsHelper, index 5).
    const Instr body[] = {CONST(1), bare(Op::REVBITS), bare(Op::RETURN)};
    FakeRuntime<1> rt;
    rt.set(0, 0, /*savesLR=*/true, body, 3);
    uint32_t halfwordCount = translateProc(0, rt.runtime(), LRU_TICK);
    CHECK(halfwordCount == 10);

    const uint16_t expected[] = {
        PROLOGUE_STUB,
        ArmV6M::pushWithLr(ArmV6M::LoRegs{0}),             // PUSH {lr}  (savesLR — REVBITS needs LR save, reached via BLX like CALL)
        ArmV6M::movs(ArmV6M::LoReg(0), ArmV6M::Imm<8>(1)), // MOVS r0, #1  (CONST 1)
        ArmV6M::mov(ArmV6M::AnyReg(3), ArmV6M::AnyReg(10)), ArmV6M::ldr(ArmV6M::LoReg(3), ArmV6M::LoReg(3), ArmV6M::Uoff<2, 5>(16)),
        ArmV6M::blx(ArmV6M::AnyReg(3)),                    // MOV r3,r10; LDR r3,[r3,#16]; BLX r3  (revbitsHelper, index 4 — BLX not BX: it returns via bx lr like an ordinary subroutine)
        RETURN_VIA_STACK,
    };
    for(uint32_t i = 0; i < halfwordCount; i++)
    {
        CHECK(rt.code()[i] == expected[i]);
    }
}

TEST(ComparisonImmediatelyBeforeBrTableJumpTableDoesNotFuse)
{
    // Fusion only applies for BR_TABLE 1 (the truthy two-way form) — a
    // comparison right before a genuine jump-table selector must
    // materialize as an ordinary 0/1 value instead, exercising the
    // "hasLookahead but op doesn't qualify" side of fusesIntoBrTable's own
    // condition.
    const Instr body[] = {
        LOAD(0), opImm(Op::GT_U, 3), brTable(2),
            CONST(10), bare(Op::BLOCK_END),
            CONST(20), bare(Op::BLOCK_END),
            CONST(30), bare(Op::BLOCK_END),
        // isa-core.md §8.7: the switch's merge point starts acc poisoned
        // regardless of which case fell through — RETURN needs its own
        // fresh producer.
        LOAD(0),
        bare(Op::RETURN),
    };
    FakeRuntime<1> rt(/*arenaBytes=*/128);
    rt.set(0, 1, /*savesLR=*/true, body, sizeof(body) / sizeof(body[0]));
    uint32_t halfwordCount = translateProc(0, rt.runtime(), LRU_TICK);
    CHECK(halfwordCount == 25);

    const uint16_t expected[] = {
        PROLOGUE_STUB,
        ArmV6M::pushWithLr(ArmV6M::LoRegs{0}),              // PUSH {lr}  (savesLR — a jump table needs LR save)
        ArmV6M::mov(ArmV6M::AnyReg(7), ArmV6M::AnyReg(0)),  // MOV r7, r0  (callee prologue: incoming last arg flushed into physReg(0)=r7)
        ArmV6M::cmp(ArmV6M::LoReg(7), ArmV6M::Imm<8>(3)),   // CMP r7, #3  (LOAD(0): ordinary in-window read of physReg(0), pending straight into the comparison; GT_U operand=3)
        ArmV6M::bls(ArmV6M::Ioff<1, 8>(2)),                 // BLS +2  (inverse of HI/GT_U's true condition — comparison doesn't fuse into a jump table)
        ArmV6M::movs(ArmV6M::LoReg(0), ArmV6M::Imm<8>(1)),  // MOVS r0, #1  (materialized true value)
        ArmV6M::b(ArmV6M::Ioff<1, 11>(0)),                  // skip the false branch
        ArmV6M::movs(ArmV6M::LoReg(0), ArmV6M::Imm<8>(0)),  // MOVS r0, #0  (materialized false value)
        ArmV6M::movs(ArmV6M::LoReg(2), ArmV6M::Imm<8>(2)),  // MOVS r2, #2  (materializeImm32(SCRATCH_REG, n=2) for openBrTableJump)
        ArmV6M::mov(ArmV6M::AnyReg(3), ArmV6M::AnyReg(10)), ArmV6M::ldr(ArmV6M::LoReg(3), ArmV6M::LoReg(3), ArmV6M::Uoff<2, 5>(20)),
        ArmV6M::blx(ArmV6M::AnyReg(3)),                     // MOV r3,r10; LDR r3,[r3,#20]; BLX r3  (brTableJumpHelper, index 5 — lr must point at the table right after)
        0x0006, // jump table slot 0 (case0 start, byte offset from table base)
        0x000a, // jump table slot 1 (case1 start)
        0x000e, // jump table slot 2 (case2 — the default case)
        ArmV6M::movs(ArmV6M::LoReg(0), ArmV6M::Imm<8>(10)), // CONST(10), case0
        ArmV6M::b(ArmV6M::Ioff<1, 11>(4)),                  // case0's skip-to-merge
        ArmV6M::movs(ArmV6M::LoReg(0), ArmV6M::Imm<8>(20)), // CONST(20), case1
        ArmV6M::b(ArmV6M::Ioff<1, 11>(0)),                  // case1's skip-to-merge
        ArmV6M::movs(ArmV6M::LoReg(0), ArmV6M::Imm<8>(30)), // CONST(30), case2 (the default — falls into the merge)
        ArmV6M::mov(ArmV6M::AnyReg(0), ArmV6M::AnyReg(7)),  // MOV r0, r7  (RETURN's flush of the post-switch LOAD(0) — the merge point's own fresh producer)
        RETURN_VIA_STACK,
    };
    for(uint32_t i = 0; i < halfwordCount; i++)
    {
        CHECK(rt.code()[i] == expected[i]);
    }
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
        // isa-core.md §8.7: the loop-exit edge starts acc poisoned, so
        // RETURN needs its own fresh producer rather than reading whatever
        // the body last left behind.
        LOAD(0),
        bare(Op::RETURN),
    };
    FakeRuntime<1> rt;
    rt.set(0, 0, /*savesLR=*/false, body, sizeof(body) / sizeof(body[0]));
    uint32_t halfwordCount = translateProc(0, rt.runtime(), LRU_TICK);
    CHECK(halfwordCount == 13);

    const uint16_t expected[] = {
        PROLOGUE_STUB,
        ArmV6M::movs(ArmV6M::LoReg(7), ArmV6M::Imm<8>(3)),              // MOVS r7, #3  (CONST 3, into physReg(0) for the PUSH)
        ArmV6M::mov(ArmV6M::AnyReg(0), ArmV6M::AnyReg(7)),              // MOV r0, r7  (openLoop's own flushLive: canonicalize acc before the condition sub-block)
        ArmV6M::cmp(ArmV6M::LoReg(7), ArmV6M::Imm<8>(0)),               // CMP r7, #0  (LOAD(0)+opImm(GT_S,0) fused: pending value read straight from r7)
        ArmV6M::ble(ArmV6M::Ioff<1, 8>(4)),                             // BLE — inverse(GT)=LE, the loop-exit branch, patched past the back-edge
        ArmV6M::subs(ArmV6M::LoReg(7), ArmV6M::LoReg(7), ArmV6M::Imm<3>(1)), // SUBS r7,r7,#1  (LOAD(0)+opImm(SUB,1) folds straight into STORE(0)'s target r7)
        ArmV6M::mov(ArmV6M::AnyReg(0), ArmV6M::AnyReg(7)),              // MOV r0, r7  (closeBlockEnd's own flushLive before the back-edge)
        ArmV6M::b(ArmV6M::Ioff<1, 11>(-12)),                            // unconditional back-edge to loopStart
        ArmV6M::mov(ArmV6M::AnyReg(0), ArmV6M::AnyReg(7)),              // MOV r0, r7  (RETURN's flush of the post-loop LOAD(0) — the exit edge's own fresh producer)
        RETURN_VIA_LR,
    };
    for(uint32_t i = 0; i < halfwordCount; i++)
    {
        CHECK(rt.code()[i] == expected[i]);
    }
}

TEST(ComparisonMaterializedResultFoldsDirectlyIntoAFollowingStore)
{
    // materializeComparison's own fold.reg>=0 path (the comparison-as-
    // value analog of CONST/arithmetic's own store-fold) — every other
    // comparison-as-value test in this file has the result land in
    // ACC_REG (fold.reg<0) instead.
    const Instr body[] = {CONST(5), opImm(Op::GT_U, 3), STORE(0), bare(Op::RETURN)};
    FakeRuntime<1> rt;
    rt.set(0, 1, /*savesLR=*/false, body, sizeof(body) / sizeof(body[0]));
    uint32_t halfwordCount = translateProc(0, rt.runtime(), LRU_TICK);
    CHECK(halfwordCount == 13);

    const uint16_t expected[] = {
        PROLOGUE_STUB,
        ArmV6M::mov(ArmV6M::AnyReg(7), ArmV6M::AnyReg(0)), // MOV r7, r0  (callee prologue: incoming last arg flushed into physReg(0))
        ArmV6M::movs(ArmV6M::LoReg(0), ArmV6M::Imm<8>(5)), // MOVS r0, #5  (CONST 5)
        ArmV6M::cmp(ArmV6M::LoReg(0), ArmV6M::Imm<8>(3)),  // CMP r0, #3  (GT_U operand=3)
        ArmV6M::bls(ArmV6M::Ioff<1, 8>(2)),                // BLS — inverse of HI/GT_U's true condition
        ArmV6M::movs(ArmV6M::LoReg(7), ArmV6M::Imm<8>(1)), // MOVS r7, #1  (materializeComparison's fold.reg path: true value straight into STORE's target r7)
        ArmV6M::b(ArmV6M::Ioff<1, 11>(0)),                 // skip the false branch
        ArmV6M::movs(ArmV6M::LoReg(7), ArmV6M::Imm<8>(0)), // MOVS r7, #0  (false value into r7)
        ArmV6M::mov(ArmV6M::AnyReg(0), ArmV6M::AnyReg(7)), // MOV r0, r7  (RETURN's own flushLive: canonicalize acc back into r0)
        RETURN_VIA_LR,
    };
    for(uint32_t i = 0; i < halfwordCount; i++)
    {
        CHECK(rt.code()[i] == expected[i]);
    }
}

TEST(LastArgumentHomeSlotReadViaRegAcc)
{
    // A REG_ACC read (opReg) of the last argument's own slot, right at
    // body start — reads via physReg(slot) rather than a LOAD, exercising
    // a different addressing mode than LastArgumentHomeSlotReadTwiceViaLoad
    // above against the same unconditionally-flushed value.
    const Instr body[] = {opReg(Op::ADD, 1), bare(Op::RETURN)};
    FakeRuntime<1> rt;
    rt.set(0, 2, /*savesLR=*/false, body, 2);
    uint32_t halfwordCount = translateProc(0, rt.runtime(), LRU_TICK);
    CHECK(halfwordCount == 7);

    const uint16_t expected[] = {
        PROLOGUE_STUB,
        ArmV6M::mov(ArmV6M::AnyReg(6), ArmV6M::AnyReg(0)), // MOV r6, r0  (callee prologue: incoming last arg flushed into physReg(1)=r6)
        ArmV6M::adds(ArmV6M::LoReg(0), ArmV6M::LoReg(6), ArmV6M::LoReg(6)), // ADDS r0,r6,r6  (opReg(ADD,1): acc, already r6 post-flush, plus window slot1 — same r6)
        RETURN_VIA_LR,
    };
    for(uint32_t i = 0; i < halfwordCount; i++)
    {
        CHECK(rt.code()[i] == expected[i]);
    }
}

static uint32_t currentSp()
{
    register uint32_t sp asm("sp");
    return sp;
}

TEST(DeeplyNestedButWellFormedBlocksSucceedWithNoStackFloor)
{
    // 50 levels of well-formed nesting, each BR_TABLE(1) case closed by its
    // own BLOCK_END. Nesting is bounded by the live stack pointer's real
    // headroom, not by an arbitrary count, and with stackFloor 0 this host
    // process has plenty.
    constexpr int kDepth = 50;
    // isa-core.md §8.7: the outermost if-then's merge point starts acc
    // poisoned once all 50 levels unwind, so RETURN needs its own fresh
    // producer (CONST — argCount=0, no window slot available).
    // Each level is a producer plus a dispatch, and closes with two
    // BLOCK_ENDs: `BR_TABLE 1` opens a case and a default case, and a case
    // starts with acc dead (isa-core.md §8.7) so the next level down has to
    // establish its own dispatch value.
    Instr body[4 * kDepth + 2];
    for(int i = 0; i < kDepth; i++)
    {
        body[2 * i] = CONST(1);
        body[2 * i + 1] = brTable(1);
    }
    for(int i = 0; i < 2 * kDepth; i++)
    {
        body[2 * kDepth + i] = bare(Op::BLOCK_END);
    }
    body[4 * kDepth] = CONST(0);
    body[4 * kDepth + 1] = bare(Op::RETURN);
    FakeRuntime<1> rt(/*arenaBytes=*/1024);
    rt.set(0, 0, /*savesLR=*/false, body, 4 * kDepth + 2, /*cap=*/512);
    translateProc(0, rt.runtime(), LRU_TICK);
}

TEST(BlockNestingReportsOverflowWhenLiveStackFloorIsUnsatisfiable)
{
    // rt's stackLimit pinned at (essentially) the current sp — no margin left
    // at all, so processUntilTerminator's guard fails on the first block,
    // regardless of how shallow the body is. Deliberately does not
    // calibrate "how many nesting levels exhaust N bytes": this build is
    // -O0, unlike the target's -Os, so the number would not transfer. It
    // proves the mechanism fires whenever the floor cannot be satisfied.
    // Pinning RESOURCE_EXHAUSTED_TRANSLATOR_STACK separates it from an
    // arena overflow, which reaches the same escape.
    const Instr body[] = {bare(Op::RETURN)};
    FakeRuntime<1> rt(/*arenaBytes=*/32, /*stackLimit=*/currentSp());
    rt.set(0, 0, /*savesLR=*/false, body, 1);

    EXPECT_RESOURCE_ERROR(RESOURCE_EXHAUSTED_TRANSLATOR_STACK, translateProc(0, rt.runtime(), LRU_TICK));
}

TEST(FlatBodySucceedsWithSlackThatOnlyCoversTranslateBodysOwnDepthZeroCheck)
{
    // Companion to NestedIfChainReportsOverflowWithTheSameSlackADepthZeroBodyTolerates
    // below — establishes the baseline half of the comparison. A floor with
    // comfortable slack beyond one guard's margin lets a body with no nesting
    // at all — a single pass through processUntilTerminator — succeed.
    const Instr body[] = {bare(Op::RETURN)};
    FakeRuntime<1> rt(/*arenaBytes=*/32, /*stackLimit=*/currentSp() - 1536); // generous slack — see the paired test below
    rt.set(0, 0, /*savesLR=*/false, body, 1);
    translateProc(0, rt.runtime(), LRU_TICK);
}

TEST(NestedIfChainReportsOverflowWithTheSameSlackADepthZeroBodyTolerates)
{
    // Proves the guard fires per recursion level, not once: the exact same
    // floor the companion test above shows a flat body tolerates comfortably
    // is exhausted
    // once real translation recurses 8 levels deep through BR_TABLE(1) ->
    // translateIfThen -> processUntilTerminator -> processNonTerminators ->
    // back into translateIfThen — each level a handful of real,
    // un-inlined -O0 stack frames. Before this fix, only translateBody's
    // one check (at depth 0) ever ran, so this exact program and floor
    // would have silently compiled the whole 8-level chain instead of
    // bailing. Deliberately doesn't try to calibrate "exactly one level" —
    // that would be exact-byte-fragile on an -O0 host build that doesn't
    // resemble the real target's -Os codegen (see the depth-0 test above's
    // own reasoning); 8 levels against 1024 bytes of slack only needs "one
    // level costs a small double-digit-or-more number of bytes," true by a
    // wide margin for a handful of un-inlined function calls.
    constexpr int kDepth = 8;
    // Each level is a producer plus a dispatch, and closes with two
    // BLOCK_ENDs: `BR_TABLE 1` opens a case and a default case, and a case
    // starts with acc dead (isa-core.md §8.7) so the next level down has to
    // establish its own dispatch value.
    Instr body[4 * kDepth + 2];
    for(int i = 0; i < kDepth; i++)
    {
        body[2 * i] = CONST(1);
        body[2 * i + 1] = brTable(1);
    }
    for(int i = 0; i < 2 * kDepth; i++)
    {
        body[2 * kDepth + i] = bare(Op::BLOCK_END);
    }
    body[4 * kDepth] = CONST(0);
    body[4 * kDepth + 1] = bare(Op::RETURN);

    FakeRuntime<1> rt(/*arenaBytes=*/1024, /*stackLimit=*/currentSp() - 1536);
    rt.set(0, 0, /*savesLR=*/false, body, 4 * kDepth + 2, /*cap=*/512);

    EXPECT_RESOURCE_ERROR(RESOURCE_EXHAUSTED_TRANSLATOR_STACK, translateProc(0, rt.runtime(), LRU_TICK));
}

// ── Literal pooling ─────────────────────────────────────────────────────

TEST(LargeConstAndLargeOperandBothPoolIntoOneChunk)
{
    // Both immediates need 7 synthesis halfwords each, so both pool. The
    // whole emitted output is checked literally: the two sites sit at byte
    // 4 and 6 — one word-aligned, one not — so between them they cover
    // both halves of Align(pc+4,4)'s rounding, resolving to the same base
    // (8) but different pool words.
    const Instr body[] = {CONST(0x12345678), opImm(Op::ADD, 0x0ABCDEF0), bare(Op::RETURN)};
    FakeRuntime<1> rt;
    rt.set(0, 0, /*savesLR=*/false, body, 3);
    uint32_t halfwordCount = translateProc(0, rt.runtime(), LRU_TICK);

    CHECK(halfwordCount == 12); // 20 unpooled: 7 + 7 synthesis halfwords

    const uint16_t expected[] = {
        PROLOGUE_STUB,
        ArmV6M::ldrPc(ArmV6M::LoReg(0), ArmV6M::Uoff<2, 8>(8)),  // LDR r0,[pc,#8]   -> byte 16
        ArmV6M::ldrPc(ArmV6M::LoReg(2), ArmV6M::Uoff<2, 8>(12)), // LDR r2,[pc,#12]  -> byte 20
        ArmV6M::adds(ArmV6M::LoReg(0), ArmV6M::LoReg(0), ArmV6M::LoReg(2)), // ADDS r0,r0,r2 — operand came from the pool
        RETURN_VIA_LR,
        0x5678, 0x1234,                                  // pool: 0x12345678
        0xDEF0, 0x0ABC,                                  // pool: 0x0ABCDEF0
    };
    for(uint32_t i = 0; i < halfwordCount; i++)
    {
        CHECK(rt.code()[i] == expected[i]);
    }
}

TEST(ShiftAmountNeverPoolsEvenWhenHardToSynthesize)
{
    // A shift's IMM_ACC operand is consumed straight as an Imm<5>, so it
    // must stay an immediate no matter what it costs to synthesize.
    const Instr body[] = {LOAD(0), opImm(Op::SHL, 3), bare(Op::RETURN)};
    FakeRuntime<1> rt;
    rt.set(0, 1, /*savesLR=*/false, body, 3);
    uint32_t halfwordCount = translateProc(0, rt.runtime(), LRU_TICK);
    CHECK(literalSiteCount(rt.code(), halfwordCount) == 0);
}

TEST(PooledLoadInsideGuardedRegionStillResolves)
{
    // One pooled site in each arm of an if/else. Each arm's own chunk
    // flushes as soon as its own case's forward fixup is resolved
    // (Assembler::bind's own flush-before-resolve ordering) rather than
    // both surviving in one chunk to the end of the procedure — but
    // either way both values must still decode correctly.
    const Instr body[] = {
        CONST(5), opImm(Op::GT_U, 3), brTable(1),
            CONST(0x12345678), bare(Op::BLOCK_END),
            CONST(0x0ABCDEF0), bare(Op::BLOCK_END),
        // isa-core.md §8.7: the if/else merge point starts acc poisoned
        // regardless of which arm ran — RETURN needs its own fresh
        // producer. A small immediate (not a literal) so it doesn't
        // disturb the pooling this test is about.
        CONST(0),
        bare(Op::RETURN),
    };
    FakeRuntime<1> rt;
    rt.set(0, 0, /*savesLR=*/false, body, sizeof(body) / sizeof(body[0]));
    uint32_t halfwordCount = translateProc(0, rt.runtime(), LRU_TICK);

    CHECK(literalSiteCount(rt.code(), halfwordCount) == 2);

    uint32_t value = 0;
    CHECK(loadedWord(rt.code(), halfwordCount, nthLiteralSite(rt.code(), halfwordCount, 0), value));
    CHECK(value == 0x12345678);
    CHECK(loadedWord(rt.code(), halfwordCount, nthLiteralSite(rt.code(), halfwordCount, 1), value));
    CHECK(value == 0x0ABCDEF0);
}

TEST(PooledLoadSurvivesAcrossABrTableJumpTableUnflushed)
{
    // Unlike the old bytecode-tag-based pool, this one tracks each
    // pending site by its own stored (site, value) pair rather than
    // scanning the output for anything that merely looks like a literal
    // load — so BR_TABLE(N>2)'s own raw jump-table halfwords are never at
    // risk of being misread, and a chunk opened before one needs no
    // forced flush to stay safe. The one CONST here survives, unflushed,
    // all the way to the end-of-procedure flush, landing *after* the
    // jump table rather than before it.
    const Instr body[] = {
        CONST(0x12345678), brTable(2),
            CONST(1), bare(Op::BLOCK_END),
            CONST(2), bare(Op::BLOCK_END),
            CONST(3), bare(Op::BLOCK_END),
        // isa-core.md §8.7: the switch's merge point starts acc poisoned
        // regardless of which case fell through — RETURN needs its own
        // fresh producer. A small immediate (not a literal) so it doesn't
        // disturb the one pooled site this test is about.
        CONST(0),
        bare(Op::RETURN),
    };
    FakeRuntime<1> rt(/*arenaBytes=*/128);
    rt.set(0, 0, /*savesLR=*/true, body, sizeof(body) / sizeof(body[0]));
    uint32_t halfwordCount = translateProc(0, rt.runtime(), LRU_TICK);

    CHECK(literalSiteCount(rt.code(), halfwordCount) == 1);

    uint32_t value = 0;
    uint32_t site = nthLiteralSite(rt.code(), halfwordCount, 0);
    CHECK(loadedWord(rt.code(), halfwordCount, site, value));
    CHECK(value == 0x12345678);
}

TEST(LargeBrTableJumpTableFlushesAPendingLiteralBeforeItsOwnTable)
{
    // A BR_TABLE(N)'s own dispatch+table span must stay contiguous (the
    // helper jumps by indexing directly off where the dispatch's own blx
    // lands), so nothing may flush while it's being emitted. For a large
    // enough N the table alone (500 halfwords here) comfortably exceeds
    // LITERAL_POOL_MAX_REACH on its own -- translateSwitch's own
    // ensurePoolRoom(0, tableBytes) call folds the table's known length in
    // *before* entering that protected span, flushing the CONST parked
    // just before it so its pool word never has to survive the table.
    // Cases are deliberately empty (bare BLOCK_END, no body of their own)
    // -- this is about the table's own size, not case-body content, and
    // each case's own branchTo(end) has a separate, much shorter reach
    // limit (+-2048 bytes) that a real per-case body would risk crossing
    // at this case count.
    constexpr uint32_t kCases = 520;
    Instr body[2 + kCases + 3];
    uint32_t n = 0;
    body[n++] = CONST(0x12345678); // the one literal this test is about
    body[n++] = brTable(kCases);
    for(uint32_t i = 0; i <= kCases; i++)
    {
        body[n++] = bare(Op::BLOCK_END);
    }
    // isa-core.md §8.7: the switch's merge point starts acc poisoned
    // regardless of which case fell through -- RETURN needs its own
    // fresh producer.
    body[n++] = CONST(0);
    body[n++] = bare(Op::RETURN);

    FakeRuntime<1> rt(/*arenaBytes=*/8192);
    rt.set(0, 0, /*savesLR=*/true, body, n, /*cap=*/2048);
    uint32_t halfwordCount = translateProc(0, rt.runtime(), LRU_TICK);

    CHECK(literalSiteCount(rt.code(), halfwordCount) == 1);
    uint32_t value = 0;
    uint32_t site = nthLiteralSite(rt.code(), halfwordCount, 0);
    CHECK(loadedWord(rt.code(), halfwordCount, site, value));
    CHECK(value == 0x12345678);
}

TEST(OutputReachDistanceForcesAMidProcedureFlush)
{
    // 500 single-halfword NOTs between two pooled sites push the first
    // one's forward reach past its 1020-byte limit, forcing a flush and a
    // fresh chunk — the reach guard is the only mid-procedure flush
    // trigger. Both loads must still reach their own word.
    Instr body[504];
    uint32_t n = 0;
    body[n++] = CONST(0x12345678);
    for(uint32_t i = 0; i < 500; i++)
    {
        body[n++] = bare(Op::NOT);
    }
    body[n++] = opImm(Op::ADD, 0x0ABCDEF0);
    body[n++] = bare(Op::RETURN);

    FakeRuntime<1> rt(/*arenaBytes=*/2048);
    rt.set(0, 0, /*savesLR=*/false, body, n, /*cap=*/1024);
    uint32_t halfwordCount = translateProc(0, rt.runtime(), LRU_TICK);

    CHECK(literalSiteCount(rt.code(), halfwordCount) == 2);

    uint32_t value = 0;
    CHECK(loadedWord(rt.code(), halfwordCount, nthLiteralSite(rt.code(), halfwordCount, 0), value));
    CHECK(value == 0x12345678);
    CHECK(loadedWord(rt.code(), halfwordCount, nthLiteralSite(rt.code(), halfwordCount, 1), value));
    CHECK(value == 0x0ABCDEF0);
}

TEST(OutputReachOverflowFlushesBeforeGoingOutOfRange)
{
    // CALL emits far more output per bytecode byte than anything else, so
    // a long run of them exhausts the load's 1020-byte forward reach well
    // before it would otherwise. Each CALL's own force-pooled record
    // (abi_strategy.cpp) also contends for pool room now, so this only
    // checks that the original CONST's own pooled value is still
    // reachable and decodes correctly — not an exact site count, which
    // now depends on how many call records also happened to still be
    // pending when it flushed.
    Instr body[128];
    uint32_t n = 0;
    body[n++] = CONST(0x12345678);
    for(uint32_t i = 0; i < 120; i++)
    {
        body[n++] = call(0);
    }
    body[n++] = bare(Op::RETURN);

    FakeRuntime<1> rt(/*arenaBytes=*/8192);
    rt.set(0, 0, /*savesLR=*/true, body, n, /*cap=*/512);
    uint32_t halfwordCount = translateProc(0, rt.runtime(), LRU_TICK);

    uint32_t site = nthLiteralSite(rt.code(), halfwordCount, 0);
    CHECK(site < halfwordCount);
    uint32_t value = 0;
    CHECK(loadedWord(rt.code(), halfwordCount, site, value));
    CHECK(value == 0x12345678);

    // Flushed within reach, wherever it ended up.
    uint16_t off;
    CHECK(ArmV6M::getLiteralOffset(rt.code()[site], off));
    CHECK(off * 4u <= 1020);
}

// ── the extension seam at codegen ────────────────────────────────────────

namespace
{
// Captured by the fake emit(), so a test can assert on what the core
// handed it rather than only on the bytes that came out.
struct SeenSite
{
    bool called = false;
    uint32_t opcodeByte = 0;
    uint32_t depthOnEntry = 0;
};
SeenSite g_seen;

uint32_t twoPopDecode(const uint8_t *, uint32_t, uint32_t, uint32_t *decl)
{
    *decl = jitc::extDecl(0, /*tosDelta=*/-2, /*halfwords=*/6);
    return 1;
}

void captureEmit(ExtSite &site)
{
    g_seen.called = true;
    g_seen.opcodeByte = *site.opcode();
    g_seen.depthOnEntry = site.depth();

    // Something real and recognisable: acc = top + next, taken off the
    // stack through the site rather than staged for it.
    site.pop(ENTRY_IDX_REG);
    site.pop(SCRATCH_REG);
    site.a.emit(ArmV6M::adds(ArmV6M::LoReg(ACC_REG), ArmV6M::LoReg(ENTRY_IDX_REG), ArmV6M::LoReg(SCRATCH_REG)));
    site.accIsNowIn(ACC_REG);
}

void overrunEmit(ExtSite &site)
{
    // Declares 6 halfwords (twoPopDecode) but emits 7.
    for(uint32_t i = 0; i < 7; i++)
    {
        site.a.emit(ArmV6M::adds(ArmV6M::LoReg(ACC_REG), ArmV6M::LoReg(ACC_REG), ArmV6M::Imm<3>(1)));
    }
}

void underPopEmit(ExtSite &site)
{
    // Declares -2 but takes only one off the stack.
    site.pop(ENTRY_IDX_REG);
    site.accIsNowIn(ENTRY_IDX_REG);
}

uint32_t helperDecode(const uint8_t *, uint32_t, uint32_t, uint32_t *decl)
{
    // Declares NEEDS_LR, which both reach forms require: a BLX clobbers lr,
    // and the prologue's decision to save it came from this flag.
    *decl = jitc::extDecl(jitc::EXT_FLAG_NEEDS_LR, /*tosDelta=*/-2, /*halfwords=*/16, /*poolWords=*/1);
    return 1;
}

constexpr uint32_t FAKE_HELPER_ADDR = 0x0800BEEFu;

void rawHelperEmit(ExtSite &site)
{
    site.pop(ENTRY_IDX_REG);
    site.pop(SCRATCH_REG);
    site.helperCall(FAKE_HELPER_ADDR);
    site.accIsNowIn(ACC_REG);
}

void cHelperEmit(ExtSite &site)
{
    site.pop(ENTRY_IDX_REG);
    site.pop(SCRATCH_REG);
    site.cHelperCall(FAKE_HELPER_ADDR);
    site.accIsNowIn(ACC_REG);
}

const ExtStub EXT_RAW_HELPER = {helperDecode, rawHelperEmit, 0};
const ExtStub EXT_C_HELPER = {helperDecode, cHelperEmit, EXT_THUNK_STACK_BYTES};

// True iff `needle` appears anywhere in the first `n` halfwords of `buf`.
bool containsSeq(const uint16_t *buf, uint32_t n, const uint16_t *needle, uint32_t len)
{
    for(uint32_t i = 0; i + len <= n; i++)
    {
        uint32_t j = 0;
        while(j < len && buf[i + j] == needle[j]) j++;
        if(j == len) return true;
    }
    return false;
}

const ExtStub EXT_CAPTURE = {twoPopDecode, captureEmit};
const ExtStub EXT_OVERRUN = {twoPopDecode, overrunEmit};
const ExtStub EXT_UNDER_POP = {twoPopDecode, underPopEmit};


// PUSH PUSH <0x80> RETURN, hand-spliced: encodeBody takes Instr[], which
// cannot express an extension op (its operands never live in Instr).
uint32_t extBody(uint8_t *out)
{
    const Instr prelude[] = {CONST(3), PUSH(), CONST(4), PUSH()};
    uint32_t n = encodeBody(prelude, 4, out, 16);
    out[n++] = 0x80;
    out[n++] = 102; // RETURN
    return n;
}

// Five pushes, so slot 0 is out of the four-register window and only
// reachable through the spill area.
uint32_t deepExtBody(uint8_t *out)
{
    const Instr prelude[] = {
        CONST(1), PUSH(), CONST(2), PUSH(), CONST(3), PUSH(),
        CONST(4), PUSH(), CONST(5), PUSH()};
    uint32_t n = encodeBody(prelude, 10, out, 48);
    out[n++] = 0x80;
    out[n++] = 102; // RETURN
    return n;
}

uint32_t deepDecode(const uint8_t *, uint32_t, uint32_t, uint32_t *decl)
{
    *decl = jitc::extDecl(0, /*tosDelta=*/0, /*halfwords=*/4);
    return 1;
}

void deepLoadEmit(ExtSite &site)
{
    uint32_t r = site.load(/*slot=*/0, ENTRY_IDX_REG);
    site.a.emit(ArmV6M::adds(ArmV6M::LoReg(ACC_REG), ArmV6M::LoReg(r), ArmV6M::Imm<3>(1)));
    site.accIsNowIn(ACC_REG);
}

void deepStoreEmit(ExtSite &site)
{
    site.accInto(ACC_REG);
    site.store(/*slot=*/0, ACC_REG);
}

const ExtStub EXT_DEEP_LOAD = {deepDecode, deepLoadEmit};
const ExtStub EXT_DEEP_STORE = {deepDecode, deepStoreEmit};
} // namespace

TEST(AnExtensionOpDrivesTheOperandStackThroughItsSite)
{
    g_seen = SeenSite{};
    FakeRuntime<1> rt(/*arenaBytes=*/128);
    uint8_t *raw = rt.bodyBuf(0, 32);
    rt.setLen(0, /*argCount=*/0, extBody(raw), /*savesLR=*/false);
    ExtScope extScope(&EXT_CAPTURE);
    uint32_t n = translateProc(0, rt.runtime(), LRU_TICK);

    CHECK(g_seen.called);
    // site.opcode() addresses the opcode byte itself, not the byte after it.
    CHECK(g_seen.opcodeByte == 0x80);
    // Both pushes are visible to the site — the core stages nothing.
    CHECK(g_seen.depthOnEntry == 2);

    const uint16_t adds[] = {
        ArmV6M::adds(ArmV6M::LoReg(ACC_REG), ArmV6M::LoReg(ENTRY_IDX_REG), ArmV6M::LoReg(SCRATCH_REG))};
    CHECK(containsSeq(rt.code(), n, adds, 1));
}

TEST(AnExtensionOpOverrunningItsDeclaredBudgetIsReported)
{
    // Caught here rather than left to surface as arena pressure and a
    // wide-branch retry, neither of which names the extension.
    FakeRuntime<1> rt(/*arenaBytes=*/128);
    uint8_t *raw = rt.bodyBuf(0, 32);
    rt.setLen(0, /*argCount=*/0, extBody(raw), /*savesLR=*/false);
    ExtScope extScope(&EXT_OVERRUN);

    EXPECT_RESOURCE_ERROR(RESOURCE_PROGRAM_EXT_UNSUPPORTED, translateProc(0, rt.runtime(), LRU_TICK));
}

TEST(AnExtensionOpContradictingItsDeclaredTosDeltaIsReported)
{
    // The wire's total_depth was validated against the declared delta, and
    // nothing re-derives it here — so the emitted effect has to match.
    FakeRuntime<1> rt(/*arenaBytes=*/128);
    uint8_t *raw = rt.bodyBuf(0, 32);
    rt.setLen(0, /*argCount=*/0, extBody(raw), /*savesLR=*/false);
    ExtScope extScope(&EXT_UNDER_POP);

    EXPECT_RESOURCE_ERROR(RESOURCE_PROGRAM_EXT_UNSUPPORTED, translateProc(0, rt.runtime(), LRU_TICK));
}

TEST(ASlotBelowTheWindowIsLoadedFromTheSpillArea)
{
    FakeRuntime<1> rt(/*arenaBytes=*/256);
    uint8_t *raw = rt.bodyBuf(0, 48);
    rt.setLen(0, /*argCount=*/0, deepExtBody(raw), /*savesLR=*/false);
    ExtScope extScope(&EXT_DEEP_LOAD);
    uint32_t n = translateProc(0, rt.runtime(), LRU_TICK);

    // tos is 5, so slot 0 is the one spilled value.
    const uint16_t ldr[] = {ArmV6M::ldrSp(ArmV6M::LoReg(ENTRY_IDX_REG), ArmV6M::Uoff<2, 8>(0))};
    CHECK(containsSeq(rt.code(), n, ldr, 1));
}

TEST(ASlotBelowTheWindowIsStoredIntoTheSpillArea)
{
    FakeRuntime<1> rt(/*arenaBytes=*/256);
    uint8_t *raw = rt.bodyBuf(0, 48);
    rt.setLen(0, /*argCount=*/0, deepExtBody(raw), /*savesLR=*/false);
    ExtScope extScope(&EXT_DEEP_STORE);
    uint32_t n = translateProc(0, rt.runtime(), LRU_TICK);

    const uint16_t str[] = {ArmV6M::strSp(ArmV6M::LoReg(ACC_REG), ArmV6M::Uoff<2, 8>(0))};
    CHECK(containsSeq(rt.code(), n, str, 1));
}

TEST(ARawHelperReachIsAPooledAddressAndABlx)
{
    FakeRuntime<1> rt(/*arenaBytes=*/256);
    uint8_t *raw = rt.bodyBuf(0, 32);
    rt.setLen(0, /*argCount=*/0, extBody(raw), /*savesLR=*/true);
    ExtScope extScope(&EXT_RAW_HELPER);
    uint32_t n = translateProc(0, rt.runtime(), LRU_TICK);

    // No r10 vector detour: the address comes from the literal pool, so the
    // reach itself is just the BLX. r3 because Thumb-1 leaves nothing else
    // free — r0-r2 hold acc and the popped operands.
    const uint16_t blx[] = {ArmV6M::blx(ArmV6M::AnyReg(ENTRY_JUMP_REG))};
    CHECK(containsSeq(rt.code(), n, blx, 1));

    // Specifically NOT through the thunk. (A bare `MOV r3,r10` would be the
    // wrong thing to look for: the procedure's own RETURN reaches the r10
    // vector with that same idiom. The thunk's marker is parking the target
    // in r12 and loading the vector at the thunk's own offset.)
    const uint16_t thunkMarker[] = {ArmV6M::mov(ArmV6M::AnyReg(12), ArmV6M::AnyReg(ENTRY_JUMP_REG))};
    CHECK(!containsSeq(rt.code(), n, thunkMarker, 1));
    const uint16_t thunkLoad[] = {ArmV6M::ldr(ArmV6M::LoReg(ENTRY_JUMP_REG), ArmV6M::LoReg(ENTRY_JUMP_REG),
        ArmV6M::Uoff<2, 5>((uint16_t)HELPER_EXT_THUNK_OFFSET))};
    CHECK(!containsSeq(rt.code(), n, thunkLoad, 1));
}

TEST(ACHelperReachGoesThroughTheThunkWithTheTargetInR12)
{
    FakeRuntime<1> rt(/*arenaBytes=*/256);
    uint8_t *raw = rt.bodyBuf(0, 32);
    rt.setLen(0, /*argCount=*/0, extBody(raw), /*savesLR=*/true);
    ExtScope extScope(&EXT_C_HELPER);
    uint32_t n = translateProc(0, rt.runtime(), LRU_TICK);

    // Target parked in r12/ip — the AAPCS scratch register, so r0-r3 stay
    // the callee's own arguments — then the thunk reached through the r10
    // vector by the same MOV/LDR/BLX idiom every other helper uses.
    const uint16_t seq[] = {
        ArmV6M::mov(ArmV6M::AnyReg(12), ArmV6M::AnyReg(ENTRY_JUMP_REG)),
        ArmV6M::mov(ArmV6M::AnyReg(ENTRY_JUMP_REG), ArmV6M::AnyReg(HELPER_VEC_REG)),
        ArmV6M::ldr(ArmV6M::LoReg(ENTRY_JUMP_REG), ArmV6M::LoReg(ENTRY_JUMP_REG),
            ArmV6M::Uoff<2, 5>((uint16_t)HELPER_EXT_THUNK_OFFSET)),
        ArmV6M::blx(ArmV6M::AnyReg(ENTRY_JUMP_REG)),
    };
    CHECK(containsSeq(rt.code(), n, seq, 4));
}

// ── the window-aliasing cases the service surface exists to hide ─────────

namespace
{
/* physReg(k) wraps every WINDOW_SIZE slots, so the register a service is
 * about to write can be the one a deferred acc is still reading. Each of
 * these bodies parks acc on a window register first, with LOAD's own
 * pending-shape path, and then has the extension write that register. */
uint32_t zeroDeltaDecode(const uint8_t *, uint32_t, uint32_t, uint32_t *decl)
{
    *decl = jitc::extDecl(0, /*tosDelta=*/0, /*halfwords=*/8);
    return 1;
}

// Four pushes, then LOAD <slot> — acc ends up pending on physReg(slot).
uint32_t windowBody(uint8_t *out, uint32_t loadSlot)
{
    const Instr prelude[] = {
        CONST(1), PUSH(), CONST(2), PUSH(), CONST(3), PUSH(), CONST(4), PUSH(),
        LOAD(loadSlot)};
    uint32_t n = encodeBody(prelude, 9, out, 48);
    out[n++] = 0x80;
    out[n++] = 102; // RETURN
    return n;
}

uint32_t inWindowLoadBody(uint8_t *out) { return windowBody(out, 2); }
uint32_t aliasingBody(uint8_t *out) { return windowBody(out, 0); }

void inWindowLoadEmit(ExtSite &site)
{
    // Slot 2 of four is in the window, so this costs no instruction at all.
    uint32_t r = site.load(/*slot=*/2, ENTRY_IDX_REG);
    site.a.emit(ArmV6M::adds(ArmV6M::LoReg(ACC_REG), ArmV6M::LoReg(r), ArmV6M::Imm<3>(1)));
    site.accIsNowIn(ACC_REG);
}

void inWindowStoreEmit(ExtSite &site)
{
    // acc is pending on physReg(2), which is exactly what this overwrites.
    site.store(/*slot=*/2, ENTRY_IDX_REG);
}

void pushPopEmit(ExtSite &site)
{
    // tos is 4 and acc is pending on physReg(0) — the same register as
    // physReg(4), which is where the push lands.
    site.push(ENTRY_IDX_REG);
    site.pop(SCRATCH_REG);
}

void invalidateEmit(ExtSite &site)
{
    site.a.emit(ArmV6M::movs(ArmV6M::LoReg(ACC_REG), ArmV6M::Imm<8>(0)));
    site.accInvalidate();
}

const ExtStub EXT_IN_WINDOW_LOAD = {zeroDeltaDecode, inWindowLoadEmit};
const ExtStub EXT_IN_WINDOW_STORE = {zeroDeltaDecode, inWindowStoreEmit};
const ExtStub EXT_PUSH_POP = {zeroDeltaDecode, pushPopEmit};
const ExtStub EXT_INVALIDATE = {zeroDeltaDecode, invalidateEmit};

uint32_t translateWindowCase(FakeRuntime<1> &rt, uint32_t (*body)(uint8_t *))
{
    uint8_t *raw = rt.bodyBuf(0, 48);
    rt.setLen(0, /*argCount=*/0, body(raw), /*savesLR=*/false);
    return translateProc(0, rt.runtime(), LRU_TICK);
}
} // namespace

TEST(AnInWindowSlotIsReachedAsARegisterWithNoLoad)
{
    FakeRuntime<1> rt(/*arenaBytes=*/256);
    ExtScope extScope(&EXT_IN_WINDOW_LOAD);
    uint32_t n = translateWindowCase(rt, inWindowLoadBody);

    // physReg(2) with tos 4, and nothing loaded from the spill area.
    const uint16_t adds[] = {
        ArmV6M::adds(ArmV6M::LoReg(ACC_REG), ArmV6M::LoReg(physReg(2)), ArmV6M::Imm<3>(1))};
    CHECK(containsSeq(rt.code(), n, adds, 1));

    const uint16_t ldr[] = {ArmV6M::ldrSp(ArmV6M::LoReg(ENTRY_IDX_REG), ArmV6M::Uoff<2, 8>(0))};
    CHECK(!containsSeq(rt.code(), n, ldr, 1));
}

TEST(AStoreResolvesAnAccumulatorLivingInTheSlotItOverwrites)
{
    FakeRuntime<1> rt(/*arenaBytes=*/256);
    ExtScope extScope(&EXT_IN_WINDOW_STORE);
    uint32_t n = translateWindowCase(rt, inWindowLoadBody);

    // The deferred acc is materialized out of physReg(2) before the store
    // clobbers it — without the first MOV the LOAD's value is simply lost.
    const uint16_t seq[] = {
        ArmV6M::mov(ArmV6M::AnyReg(ACC_REG), ArmV6M::AnyReg(physReg(2))),
        ArmV6M::mov(ArmV6M::AnyReg(physReg(2)), ArmV6M::AnyReg(ENTRY_IDX_REG)),
    };
    CHECK(containsSeq(rt.code(), n, seq, 2));
}

TEST(APushResolvesAnAccumulatorAliasingItsDestination)
{
    FakeRuntime<1> rt(/*arenaBytes=*/256);
    ExtScope extScope(&EXT_PUSH_POP);
    uint32_t n = translateWindowCase(rt, aliasingBody);

    // physReg(4) == physReg(0): the push evicts that register to the spill
    // area and then overwrites it, so the acc pending on it has to be
    // materialized in between.
    CHECK(physReg(4) == physReg(0));

    const uint16_t seq[] = {
        ArmV6M::mov(ArmV6M::AnyReg(ACC_REG), ArmV6M::AnyReg(physReg(0))),
        ArmV6M::mov(ArmV6M::AnyReg(physReg(0)), ArmV6M::AnyReg(ENTRY_IDX_REG)),
    };
    CHECK(containsSeq(rt.code(), n, seq, 2));

    // And the pop uncovers what the push evicted, into that same register.
    ArmV6M::LoRegs evicted{0};
    evicted.add(ArmV6M::LoReg((uint16_t)physReg(0)));
    const uint16_t evict[] = {ArmV6M::push(evicted)};
    const uint16_t uncover[] = {ArmV6M::pop(evicted)};
    CHECK(containsSeq(rt.code(), n, evict, 1));
    CHECK(containsSeq(rt.code(), n, uncover, 1));
}

TEST(AnExtensionOpCanLeaveTheAccumulatorUndefined)
{
    // accInvalidate is the escape hatch for an op that clobbers r0 without
    // establishing a value there; the following CONST re-establishes one.
    FakeRuntime<1> rt(/*arenaBytes=*/256);
    uint8_t *raw = rt.bodyBuf(0, 48);
    const Instr prelude[] = {CONST(1), PUSH(), CONST(2), PUSH()};
    uint32_t len = encodeBody(prelude, 4, raw, 48);
    raw[len++] = 0x80;
    const Instr tail[] = {CONST(9), bare(Op::RETURN)};
    len += encodeBody(tail, 2, raw + len, 48 - len);
    rt.setLen(0, /*argCount=*/0, len, /*savesLR=*/false);

    ExtScope extScope(&EXT_INVALIDATE);
    uint32_t n = translateProc(0, rt.runtime(), LRU_TICK);
    CHECK(n > 0);
}

// ── isa-core.md §4.5's dispatch ─────────────────────────────────────────
//
// `BR_TABLE 1` is a truthy two-way test: one compare, one branch, no range
// check for a third outcome that cannot happen. Wider tables go through the
// jump-table helper instead, which costs the table itself plus the call into
// it — so this pins the shape of the two paths, not a particular encoding.

static uint32_t dispatchHalfwords(uint32_t n)
{
    // Every block stores, so acc is dead at the merge either way and the
    // only difference between the two measurements is the dispatch itself.
    Instr body[16];
    uint32_t k = 0;
    body[k++] = LOAD(0);
    body[k++] = brTable(n);
    for(uint32_t i = 0; i <= n; i++)
    {
        body[k++] = CONST((int32_t)(5 + i));
        body[k++] = STORE(1);
        body[k++] = bare(Op::BLOCK_END);
    }
    body[k++] = LOAD(1);
    body[k++] = bare(Op::RETURN);

    FakeRuntime<1> rt;
    rt.set(0, 2, /*savesLR=*/n >= 2, body, k);
    return translateProc(0, rt.runtime(), LRU_TICK);
}

TEST(TheTwoBlockDispatchIsCheaperThanTheJumpTable)
{
    const uint32_t twoBlock = dispatchHalfwords(1);
    const uint32_t jumpTable = dispatchHalfwords(2);

    CHECK(twoBlock > 0);
    CHECK(jumpTable > twoBlock);
}

TEST(DispatchCarriesAccOutOfEveryArm)
{
    // Neither arm stores anywhere, and the ADD after the merge reads what
    // the taken arm left in acc (isa-core.md §8.7).
    const Instr body[] = {
        LOAD(0), opImm(Op::GT_U, 3), brTable(1),
            CONST(11), bare(Op::BLOCK_END),
            CONST(22), bare(Op::BLOCK_END),
        opImm(Op::ADD, 1), bare(Op::RETURN),
    };
    FakeRuntime<1> rt;
    rt.set(0, 1, /*savesLR=*/false, body, sizeof(body) / sizeof(body[0]));

    CHECK(translateProc(0, rt.runtime(), LRU_TICK) > 0);
}

TEST(FallthroughEmitsNoBranchOutOfTheCase)
{
    // A case closed by FALLTHROUGH runs on into the next one, which is
    // emitted immediately after it — so it needs no branch to the merge,
    // and is one halfword shorter than the same case closed by BLOCK_END.
    auto measure = [](Op closer)
    {
        const Instr body[] = {
            CONST(0), PUSH(),
            LOAD(0), brTable(1),
                CONST(5), STORE(1), bare(closer),
                CONST(6), STORE(1), bare(Op::BLOCK_END),
            LOAD(1), bare(Op::RETURN),
        };
        FakeRuntime<1> rt;
        rt.set(0, 1, /*savesLR=*/false, body, sizeof(body) / sizeof(body[0]));
        return translateProc(0, rt.runtime(), LRU_TICK);
    };

    const uint32_t falling = measure(Op::FALLTHROUGH);
    const uint32_t leaving = measure(Op::BLOCK_END);

    CHECK(falling > 0);
    CHECK(leaving == falling + 1); // the branch to the merge FALLTHROUGH omits
}
