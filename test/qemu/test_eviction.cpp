// Arena eviction: what happens when the code arena cannot hold every
// procedure a program needs at once — victim selection, compaction, and the
// one shape that has no victim to pick.

#include <cstdint>

#include "instr.h"
#include "encode_instr.h"
#include "measure_proc.h"
#include "executor.h"
#include "dispatch_abi.h"
#include "Test.h"
#include "semihosting_output.h"

using namespace jitc;

extern "C" uint8_t __bss_end;

static constexpr uint32_t STACK_SLACK_ABOVE_BSS = 128;

static uint32_t stackLimitAboveBss()
{
    return (uint32_t)(uintptr_t)&__bss_end + STACK_SLACK_ABOVE_BSS;
}

static constexpr uint32_t SHARED_ARENA_CAPACITY = 512;
static uint8_t sharedArena[SHARED_ARENA_CAPACITY];

static ProgramResult enterProgramWithSharedArena(
    uint32_t *args, uint32_t argCount,
    const uint8_t *programBytes, uint32_t programSize, uint32_t arenaSize)
{
    return Executor::split((uint32_t)(uintptr_t)sharedArena, arenaSize, stackLimitAboveBss(), /*interruptReserve=*/0)
        .run(bcMapped(programBytes), programSize, args, argCount);
}

TEST(EvictionThreeDeepCallChain)
{
    const Instr proc0Body[] = {CONST(5), call(1), bare(Op::RETURN)};
    const Instr proc1Body[] = {LOAD(0), call(2), opImm(Op::ADD, 1), bare(Op::RETURN)};
    const Instr proc2Body[] = {LOAD(0), opImm(Op::ADD, 100), bare(Op::RETURN)};
    uint8_t bytes0[16], bytes1[16], bytes2[16];
    Proc procs[] = {
        makeProc(0, proc0Body, 3, bytes0, sizeof(bytes0)),
        makeProc(1, proc1Body, 4, bytes1, sizeof(bytes1)),
        makeProc(1, proc2Body, 3, bytes2, sizeof(bytes2)),
    };
    uint32_t argCounts[] = {0, 1, 1};

    bool savesLR[] = {true, true, false}; // proc0Body/proc1Body each CALL; proc2Body doesn't
    uint32_t sizes[3];
    uint32_t total = 0, smallest = UINT32_MAX;
    for(uint32_t i = 0; i < 3; i++)
    {
        sizes[i] = measuredHalfwords(procs[i], i, argCounts, 3, savesLR[i]) * 2;
        total += sizes[i];
        if(sizes[i] < smallest)
        {
            smallest = sizes[i];
        }
    }
    uint32_t arenaSize = total - smallest + 4; // fits any single one, but not all three

    ProcSource procSources[] = {{0, proc0Body, 3}, {1, proc1Body, 4}, {1, proc2Body, 3}};
    uint8_t progBytes[64];
    uint32_t progLen = encodeJitProgram(0, 0, procSources, 3, progBytes, sizeof(progBytes));
    ProgramResult r = enterProgramWithSharedArena(nullptr, 0, progBytes, progLen, arenaSize);

    if(r.trapped)
    {
        writeHexTrap(r.value);
    }
    CHECK(!r.trapped);
    CHECK(r.value == 106);
}

TEST(EvictionCallerAndCalleeNeverCoresident)
{
    // A calls B; the arena fits only one of the two at a time, so
    // compiling B evicts A (still suspended on the control stack, mid-
    // call) — then B's own RETURN has to recompile A from scratch before
    // it can resume. Cross-recompilation in both directions within a
    // single call/return round trip, not just one.
    const Instr proc0Body[] = {CONST(1), call(1), opImm(Op::ADD, 1000), bare(Op::RETURN)};
    const Instr proc1Body[] = {LOAD(0), opImm(Op::ADD, 1), bare(Op::RETURN)};
    uint8_t bytes0[16], bytes1[16];
    Proc procs[] = {
        makeProc(0, proc0Body, 4, bytes0, sizeof(bytes0)),
        makeProc(1, proc1Body, 3, bytes1, sizeof(bytes1)),
    };
    uint32_t argCounts[] = {0, 1};

    uint32_t size0 = measuredHalfwords(procs[0], 0, argCounts, 2, /*savesLR=*/true) * 2; // proc0Body CALLs
    uint32_t size1 = measuredHalfwords(procs[1], 1, argCounts, 2, /*savesLR=*/false) * 2; // proc1Body doesn't CALL
    uint32_t arenaSize = (size0 > size1 ? size0 : size1) + 4; // fits at most one of the two at a time

    ProcSource procSources[] = {{0, proc0Body, 4}, {1, proc1Body, 3}};
    uint8_t progBytes[48];
    uint32_t progLen = encodeJitProgram(0, 0, procSources, 2, progBytes, sizeof(progBytes));
    ProgramResult r = enterProgramWithSharedArena(nullptr, 0, progBytes, progLen, arenaSize);

    if(r.trapped)
    {
        writeHexTrap(r.value);
    }
    CHECK(!r.trapped);
    // B: 1 + 1 = 2; A: 2 + 1000 = 1002.
    CHECK(r.value == 1002);
}

TEST(EvictionSlidesAProcedureHoldingAPooledLiteral)
{
    // The one test that actually exercises PC-relative literal addressing
    // against real runtime addresses rather than translation-time layout.
    //
    // Both procedures carry pooled 32-bit literals, and the arena fits
    // only one at a time — so compiling B evicts A, and A's own RETURN
    // recompiles it, each time landing at a different arena address.
    // Every LDR [pc,#imm] offset is resolved procedure-relative at
    // translation time, so it stays correct only because Runtime::allocate
    // starts every procedure word-aligned and reserves whole words (making
    // each compaction slide a multiple of 4). Drop either half and these
    // loads read the wrong word — silently, with no trap.
    const Instr proc0Body[] = {CONST(0x12345678), call(1), opImm(Op::ADD, 0x11111111), bare(Op::RETURN)};
    const Instr proc1Body[] = {LOAD(0), opImm(Op::XOR, 0x0F0F0F0F), bare(Op::RETURN)};
    uint8_t bytes0[24], bytes1[24];
    Proc procs[] = {
        makeProc(0, proc0Body, 4, bytes0, sizeof(bytes0)),
        makeProc(1, proc1Body, 3, bytes1, sizeof(bytes1)),
    };
    uint32_t argCounts[] = {0, 1};

    uint32_t size0 = measuredHalfwords(procs[0], 0, argCounts, 2, /*savesLR=*/true) * 2; // proc0Body CALLs
    uint32_t size1 = measuredHalfwords(procs[1], 1, argCounts, 2, /*savesLR=*/false) * 2; // proc1Body doesn't CALL
    uint32_t arenaSize = (size0 > size1 ? size0 : size1) + 4; // fits at most one at a time

    ProcSource procSources[] = {{0, proc0Body, 4}, {1, proc1Body, 3}};
    uint8_t progBytes[48];
    uint32_t progLen = encodeJitProgram(0, 0, procSources, 2, progBytes, sizeof(progBytes));
    ProgramResult r = enterProgramWithSharedArena(nullptr, 0, progBytes, progLen, arenaSize);

    if(r.trapped)
    {
        writeHexTrap(r.value);
    }
    CHECK(!r.trapped);
    // B: 0x12345678 ^ 0x0F0F0F0F = 0x1D3B5977. A: + 0x11111111 = 0x2E4C6A88.
    CHECK(r.value == 0x2E4C6A88u);
}

TEST(ResourceErrorSingleProcedureLargerThanArena)
{
    // 41 arithmetic instructions is comfortably beyond any arena worth
    // testing against below — no eviction victim can ever free enough room
    // for a procedure that's bigger than the entire arena.
    Instr body[42];
    body[0] = CONST(0);
    for(int i = 1; i <= 40; i++)
    {
        body[i] = opImm(Op::ADD, 1);
    }
    body[41] = bare(Op::RETURN);
    uint8_t bytes[256];
    Proc proc = makeProc(0, body, 42, bytes, sizeof(bytes));
    uint32_t argCounts[] = {0};

    uint32_t size = measuredHalfwords(proc, 0, argCounts, 1, /*savesLR=*/false) * 2; // plain arithmetic body, no CALL
    uint32_t arenaSize = size > 24 ? size - 24 : 4; // deliberately smaller than this one procedure's own size

    ProcSource procSources[] = {{0, body, 42}};
    uint8_t progBytes[256];
    uint32_t progLen = encodeJitProgram(0, 0, procSources, 1, progBytes, sizeof(progBytes));
    ProgramResult r = enterProgramWithSharedArena(nullptr, 0, progBytes, progLen, arenaSize);

    if(r.trapped)
    {
        writeHexTrap(r.value);
    }
    else
    {
        writeHexResult(r.value);
    }
    CHECK(r.trapped);
    CHECK(r.value == RESOURCE_EXHAUSTED_ARENA);
}

TEST(EvictionChurnUnderLoopedCallChain)
{
    // Same shape as EvictionThreeDeepCallChain, but the 4-deep callee
    // chain is invoked repeatedly from inside proc0's own LOOP instead of
    // once. The arena is sized to fit only the two smallest of the five
    // procedures at a time, so every iteration but (at most) the first has
    // to evict and recompile something — exercising findEvictionVictim/
    // evict across many rounds, where every other eviction TEST here
    // evicts exactly once.
    const Instr proc0Body[] = {
        LOAD(0), PUSH(),  // k1 = counter := L
        CONST(0), PUSH(), // k2 = total := 0
        bare(Op::LOOP_PRE),
            CONST(1), call(1), opReg(Op::ADD, 2), STORE(2), // total += proc1(1)
            LOAD(1), opImm(Op::SUB, 1), STORE(1),
        bare(Op::BLOCK_END),
            LOAD(1),
        bare(Op::BLOCK_END), // back-edge while counter != 0
        LOAD(2), bare(Op::RETURN),
    };
    const Instr proc1Body[] = {LOAD(0), call(2), opImm(Op::ADD, 1), bare(Op::RETURN)};
    const Instr proc2Body[] = {LOAD(0), call(3), opImm(Op::ADD, 10), bare(Op::RETURN)};
    const Instr proc3Body[] = {LOAD(0), call(4), opImm(Op::ADD, 100), bare(Op::RETURN)};
    const Instr proc4Body[] = {LOAD(0), opImm(Op::ADD, 1000), bare(Op::RETURN)};
    uint32_t n0 = sizeof(proc0Body) / sizeof(proc0Body[0]);
    uint32_t n1 = sizeof(proc1Body) / sizeof(proc1Body[0]);
    uint32_t n2 = sizeof(proc2Body) / sizeof(proc2Body[0]);
    uint32_t n3 = sizeof(proc3Body) / sizeof(proc3Body[0]);
    uint32_t n4 = sizeof(proc4Body) / sizeof(proc4Body[0]);

    uint8_t b0[48], b1[16], b2[16], b3[16], b4[16];
    Proc procs[] = {
        makeProc(1, proc0Body, n0, b0, sizeof(b0)),
        makeProc(1, proc1Body, n1, b1, sizeof(b1)),
        makeProc(1, proc2Body, n2, b2, sizeof(b2)),
        makeProc(1, proc3Body, n3, b3, sizeof(b3)),
        makeProc(1, proc4Body, n4, b4, sizeof(b4)),
    };
    uint32_t argCounts[] = {1, 1, 1, 1, 1};
    bool savesLR[] = {true, true, true, true, false}; // proc0..proc3 each CALL; proc4 doesn't
    uint32_t sizes[5];
    for(uint32_t i = 0; i < 5; i++)
    {
        sizes[i] = measuredHalfwords(procs[i], i, argCounts, 5, savesLR[i]) * 2;
    }
    // Insertion sort (5 elements) to find the two smallest sizes — an
    // arena that fits exactly those two forces every third resident
    // procedure to evict something.
    for(uint32_t i = 1; i < 5; i++)
    {
        uint32_t v = sizes[i];
        uint32_t j = i;
        while(j > 0 && sizes[j - 1] > v)
        {
            sizes[j] = sizes[j - 1];
            j--;
        }
        sizes[j] = v;
    }
    uint32_t arenaSize = sizes[0] + sizes[1] + 4;
    // Floor: the largest procedure must fit on its own, or this traps
    // EXHAUSTED_ARENA with nothing to evict — a different scenario. The
    // two-smallest sum cleared that by only 2 bytes, so any small codegen
    // size change silently swapped the test out. Churn is unaffected.
    if(arenaSize < sizes[4] + 4)
    {
        arenaSize = sizes[4] + 4;
    }

    ProcSource procSources[] = {
        {1, proc0Body, n0}, {1, proc1Body, n1}, {1, proc2Body, n2}, {1, proc3Body, n3}, {1, proc4Body, n4},
    };
    uint8_t progBytes[160];
    uint32_t progLen = encodeJitProgram(0, 0, procSources, 5, progBytes, sizeof(progBytes));

    static constexpr uint32_t L = 4;
    // Every procedure here declares argCount 1 (argCounts above), the entry
    // one included, so L travels as a one-element vector rather than a bare
    // word.
    uint32_t entryArgs[] = {L};
    ProgramResult r = enterProgramWithSharedArena(entryArgs, 1, progBytes, progLen, arenaSize);

    if(r.trapped)
    {
        writeHexTrap(r.value);
    }
    CHECK(!r.trapped);
    // Each pass through the chain contributes 1112 (proc1(1)=1112, see the
    // per-proc bodies above); L=4 passes accumulate 4448.
    CHECK(r.value == 1112u * L);
}
