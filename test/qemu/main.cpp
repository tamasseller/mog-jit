#include <stdint.h>
#include <cassert>
#include "fixtures.h"
#include "ext.h"
#include "instr.h"
#include "encode_instr.h"
#include "proc.h"
#include "translate_proc.h"
#include "runtime.h" 
#include "dispatch_abi.h"
#include "Test.h"
#include "semihosting_output.h"
#include "stack_paint.h"

using namespace jitc;

extern "C" uint8_t __bss_end; 

static constexpr uint32_t GENEROUS_ARENA = 400;
static constexpr uint32_t GENEROUS_SLACK = 128;

static uint32_t currentSp()
{
    register uint32_t sp asm("sp");
    return sp;
}

static uint32_t stackLimitAboveBss()
{
    return (uint32_t)(uintptr_t)&__bss_end + GENEROUS_SLACK;
}

static constexpr uint32_t SHARED_ARENA_CAPACITY = 512;
static uint8_t sharedArena[SHARED_ARENA_CAPACITY];

static ProgramResult enterProgramWithSharedArena(
    uint32_t *args, uint32_t argCount,
    const uint8_t *programBytes, uint32_t programSize, uint32_t arenaSize)
{
    return enterProgramSplit(args, argCount, programBytes, programSize,
        (uint32_t)(uintptr_t)sharedArena, arenaSize, stackLimitAboveBss(), /*interruptReserve=*/0);
}

static uint32_t makeProgram(uint32_t maxCallDepth, uint32_t totalDepth, const ProcSource *procs, uint32_t procCount, uint8_t *out, uint32_t outCap)
{
    return encodeJitProgram(maxCallDepth, totalDepth, procs, procCount, out, outCap);
}

static Proc makeProc(uint32_t argCount, const Instr *body, uint32_t count, uint8_t *bytesOut, uint32_t bytesCap)
{
    uint32_t len = encodeBody(body, count, bytesOut, bytesCap);
    return Proc{argCount, bytesOut, len};
}

TEST(HandTranscribedFixturesMatchExpectedResults)
{
    bool allOk = true;

    for(uint32_t f = 0; f < fixtureCount; f++)
    {
        const Fixture &fx = fixtures[f];

        uint32_t argv[fx.program->entryArgCount];
        if(fx.args != nullptr)
        {
            for(uint32_t i = 0; i < fx.program->entryArgCount; i++)
            {
                argv[i] = fx.args[i];
            }
        }
        else if(fx.program->entryArgCount != 0)
        {
            argv[0] = fx.argIn; // the one-argument case (fixtures.h): args is for two or more
        }

        ProgramResult r = enterProgramWithSharedArena(argv, fx.program->entryArgCount,
            fx.program->bytes, fx.program->size, fx.arenaSize);

        bool ok = r.trapped == fx.expectLanding && r.value == fx.expectValue;
        allOk = allOk && ok;

        if(!ok)
        {
            semihostingWrite0(fx.name);
            semihostingWrite0(": ");
            if(r.trapped)
            {
                writeHexTrap(r.value);
            }
            else
            {
                writeHexResult(r.value);
            }
        }
    }

    CHECK(allOk);
}

TEST(OnStackGenerousSucceeds)
{
    const Instr proc0Body[] = {CONST(37), call(1), bare(Op::RETURN)};
    const Instr proc1Body[] = {LOAD(0), opImm(Op::ADD, 5), bare(Op::RETURN)};
    ProcSource procs[] = {{0, proc0Body, 3}, {1, proc1Body, 3}};
    uint8_t bytes[32];
    uint32_t len = makeProgram(/*maxCallDepth=*/1, /*totalDepth=*/1, procs, 2, bytes, sizeof(bytes));

    uint32_t stackLimit = stackLimitAboveBss();
    ProgramResult r = enterProgramOnStack(nullptr, 0, bytes, len, GENEROUS_ARENA, stackLimit, /*interruptReserve=*/0);

    if(r.trapped)
    {
        writeHexTrap(r.value);
    }
    CHECK(!r.trapped);
    CHECK(r.value == 42);
}

TEST(SplitThreeDeepCallChainSucceeds)
{
    const Instr proc0Body[] = {CONST(5), call(1), bare(Op::RETURN)};
    const Instr proc1Body[] = {LOAD(0), call(2), opImm(Op::ADD, 1), bare(Op::RETURN)};
    const Instr proc2Body[] = {LOAD(0), opImm(Op::ADD, 100), bare(Op::RETURN)};
    ProcSource procs[] = {{0, proc0Body, 3}, {1, proc1Body, 4}, {1, proc2Body, 3}};
    uint8_t bytes[48];
    uint32_t len = makeProgram(/*maxCallDepth=*/2, /*totalDepth=*/2, procs, 3, bytes, sizeof(bytes));

    static uint8_t arena[GENEROUS_ARENA];
    uint32_t stackLimit = stackLimitAboveBss();
    ProgramResult r = enterProgramSplit(nullptr, 0, bytes, len,
        (uint32_t)(uintptr_t)arena, GENEROUS_ARENA, stackLimit, /*interruptReserve=*/0);

    if(r.trapped)
    {
        writeHexTrap(r.value);
    }
    CHECK(!r.trapped);
    CHECK(r.value == 106);
}

TEST(OnStackRejectsBeforeTouchingAnything)
{
    const Instr proc0Body[] = {CONST(37), call(1), bare(Op::RETURN)};
    const Instr proc1Body[] = {LOAD(0), opImm(Op::ADD, 5), bare(Op::RETURN)};
    ProcSource procs[] = {{0, proc0Body, 3}, {1, proc1Body, 3}};
    uint8_t bytes[32];
    uint32_t len = makeProgram(/*maxCallDepth=*/1, /*totalDepth=*/1, procs, 2, bytes, sizeof(bytes));

    uint32_t stackLimit = currentSp(); // measured before this callee's own prologue — strictly higher than sp once inside it
    ProgramResult r = enterProgramOnStack(nullptr, 0, bytes, len, GENEROUS_ARENA, stackLimit, /*interruptReserve=*/0);

    if(r.trapped)
    {
        writeHexTrap(r.value);
    }
    else
    {
        writeHexResult(r.value);
    }
    CHECK(r.trapped);
    CHECK(r.value == RESOURCE_EXHAUSTED_STACK_BUDGET);
}

TEST(AProgramWithNoProceduresIsRejected)
{
    const uint8_t bytes[] = {0x00, 0x00, 0x00};
    ProgramResult r = enterProgramOnStack(nullptr, 0, bytes, sizeof(bytes), GENEROUS_ARENA, 0, /*interruptReserve=*/0);

    CHECK(r.trapped);
    CHECK(r.value == RESOURCE_PROGRAM_NO_PROCS);
}

TEST(AnExtensionRangeOpcodeIsRejectedOnHardware)
{
    const uint8_t bytes[] = {0x01, 0x01, 0x01, 0x00, 0x80};
    ProgramResult r = enterProgramOnStack(nullptr, 0, bytes, sizeof(bytes), GENEROUS_ARENA, 0, /*interruptReserve=*/0);

    CHECK(r.trapped);
    CHECK(r.value == RESOURCE_PROGRAM_EXT_UNKNOWN);
}

static uint32_t measuredHalfwords(const Proc &proc, uint32_t procIdx, const uint32_t *calleeArgCounts, uint32_t calleeCount, bool savesLR)
{
    static uint16_t scratch[128];

    alignas(8) uint8_t runtimeBytes[sizeof(Runtime) + (calleeCount + 1) * sizeof(ProcSlot)] = {};
    Runtime &r = *new(runtimeBytes) Runtime(calleeCount, (uint32_t)(uintptr_t)scratch, sizeof(scratch),
        /*stackLimit=*/0, /*arenaOverlapsStack=*/0);
    for(uint32_t i = 0; i < calleeCount; i++)
    {
        r.slot(i).codePtr = trampolineAddr;
        r.slot(i).setStaticInfo(calleeArgCounts[i], /*bodyBytes=*/0, i == procIdx && savesLR);
    }
    r.slot(procIdx).bodyPtr = (uint32_t)(uintptr_t)proc.body;
    r.slot(procIdx).setStaticInfo(proc.argCount, proc.bodyBytes, savesLR);

    return translateProc(procIdx, r, /*lruTick=*/0);
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
    uint32_t progLen = makeProgram(0, 0, procSources, 3, progBytes, sizeof(progBytes));
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
    uint32_t progLen = makeProgram(0, 0, procSources, 2, progBytes, sizeof(progBytes));
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
    uint32_t progLen = makeProgram(0, 0, procSources, 2, progBytes, sizeof(progBytes));
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
    uint32_t progLen = makeProgram(0, 0, procSources, 1, progBytes, sizeof(progBytes));
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
    // evict across many rounds, where every existing eviction fixture
    // evicts exactly once.
    const Instr proc0Body[] = {
        LOAD(0), PUSH(),  // k1 = counter := L
        CONST(0), PUSH(), // k2 = total := 0
        bare(Op::LOOP),
            LOAD(1),
        bare(Op::BLOCK_END), // while(counter != 0)
            CONST(1), call(1), opReg(Op::ADD, 2), STORE(2), // total += proc1(1)
            LOAD(1), opImm(Op::SUB, 1), STORE(1),
        bare(Op::BLOCK_END), // back-edge
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
    uint32_t progLen = makeProgram(0, 0, procSources, 5, progBytes, sizeof(progBytes));

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

// requiredStackBytes, reproduced from enter_program.cpp's own static
// function of the same name (not exported — computed here from the same
// public Runtime::storageBytesFor and dispatch_abi.h constants it uses) so
// the two boundary TESTs below can derive stackLimit from the exact
// formula the real upfront check applies, rather than the deliberately
// generous stackLimitAboveBss() every other enterProgramOnStack TEST here
// relies on. fixtures.cpp's own finishProgram comment flags this as the
// one thing nothing here yet exercises: real, hand-derived max_call_depth/
// total_depth values pushed right up against the computed floor.
static uint32_t requiredStackBytesFor(uint32_t procCount, uint32_t totalDepth, uint32_t maxCallDepth)
{
    return Runtime::storageBytesFor(procCount)
         + totalDepth * 4
         + maxCallDepth * CALL_RECORD_BYTES
         + ENTER_DISPATCH_FIXED_BYTES
         + ENTER_PROGRAM_CORE_FRAME_BYTES
         + TRANSLATOR_ENTRY_WORST_CASE_BYTES;
}

// A margin comfortably larger than the handful of stack frames between
// where this TEST measures currentSp() and where enterProgramOnStack's own
// stackHasRoom() re-measures it a few calls deeper — large enough to
// absorb that gap reliably, small enough (versus TRANSLATOR_ENTRY_WORST_
// CASE_BYTES=488 alone) that the boundary is still meaningfully tight
// rather than arbitrarily generous.
static constexpr uint32_t BOUNDARY_SLACK = 256;

TEST(OnStackAcceptsAtComputedBudgetBoundary)
{
    // The same 3-deep chain as SplitThreeDeepCallChainSucceeds
    // (maxCallDepth=2, totalDepth=2, procCount=3), with stackLimit set
    // just under the real computed floor.
    const Instr proc0Body[] = {CONST(5), call(1), bare(Op::RETURN)};
    const Instr proc1Body[] = {LOAD(0), call(2), opImm(Op::ADD, 1), bare(Op::RETURN)};
    const Instr proc2Body[] = {LOAD(0), opImm(Op::ADD, 100), bare(Op::RETURN)};
    ProcSource procs[] = {{0, proc0Body, 3}, {1, proc1Body, 4}, {1, proc2Body, 3}};
    uint8_t bytes[48];
    uint32_t maxCallDepth = 2, totalDepth = 2, procCount = 3;
    uint32_t len = makeProgram(maxCallDepth, totalDepth, procs, procCount, bytes, sizeof(bytes));

    uint32_t needed = requiredStackBytesFor(procCount, totalDepth, maxCallDepth) + GENEROUS_ARENA;
    uint32_t stackLimit = currentSp() - needed - BOUNDARY_SLACK;

    ProgramResult r = enterProgramOnStack(nullptr, 0, bytes, len, GENEROUS_ARENA, stackLimit, /*interruptReserve=*/0);

    if(r.trapped)
    {
        writeHexTrap(r.value);
    }
    CHECK(!r.trapped);
    CHECK(r.value == 106);
}

TEST(OnStackRejectsJustAboveComputedBudget)
{
    // Same program and formula as above, but stackLimit sits just above
    // the computed floor instead of just below it — the upfront check
    // should reject based on the real arithmetic, not the trivial
    // stackLimit==currentSp() case OnStackRejectsBeforeTouchingAnything
    // already covers.
    const Instr proc0Body[] = {CONST(5), call(1), bare(Op::RETURN)};
    const Instr proc1Body[] = {LOAD(0), call(2), opImm(Op::ADD, 1), bare(Op::RETURN)};
    const Instr proc2Body[] = {LOAD(0), opImm(Op::ADD, 100), bare(Op::RETURN)};
    ProcSource procs[] = {{0, proc0Body, 3}, {1, proc1Body, 4}, {1, proc2Body, 3}};
    uint8_t bytes[48];
    uint32_t maxCallDepth = 2, totalDepth = 2, procCount = 3;
    uint32_t len = makeProgram(maxCallDepth, totalDepth, procs, procCount, bytes, sizeof(bytes));

    uint32_t needed = requiredStackBytesFor(procCount, totalDepth, maxCallDepth) + GENEROUS_ARENA;
    uint32_t stackLimit = currentSp() - needed + BOUNDARY_SLACK;

    ProgramResult r = enterProgramOnStack(nullptr, 0, bytes, len, GENEROUS_ARENA, stackLimit, /*interruptReserve=*/0);

    if(r.trapped)
    {
        writeHexTrap(r.value);
    }
    else
    {
        writeHexResult(r.value);
    }
    CHECK(r.trapped);
    CHECK(r.value == RESOURCE_EXHAUSTED_STACK_BUDGET);
}

TEST(OnStackSucceedsWithBothArenaAndStackBudgetTight)
{
    // Runtime::pushStackFloor() (runtime.cpp): enterProgramOnStack
    // anchors the code arena's own base at stackLimit itself, so
    // arenaCursor advances past stackLimit as soon as even one procedure
    // compiles — at that point the translator's own live-recursion floor
    // tracks arenaCursor instead of the flat stackLimit. Every other
    // enterProgramOnStack TEST here crosses that line incidentally (a
    // generous arena/stack budget just makes it harmless); this one makes
    // both budgets tight at once — the arena just barely fits the whole
    // chain, and stackLimit sits right at the formula's own computed
    // floor — so the crossing actually matters to whether this succeeds.
    const Instr proc0Body[] = {CONST(5), call(1), bare(Op::RETURN)};
    const Instr proc1Body[] = {LOAD(0), call(2), opImm(Op::ADD, 1), bare(Op::RETURN)};
    const Instr proc2Body[] = {LOAD(0), opImm(Op::ADD, 100), bare(Op::RETURN)};
    uint8_t measureBytes0[16], measureBytes1[16], measureBytes2[16];
    Proc measureProcs[] = {
        makeProc(0, proc0Body, 3, measureBytes0, sizeof(measureBytes0)),
        makeProc(1, proc1Body, 4, measureBytes1, sizeof(measureBytes1)),
        makeProc(1, proc2Body, 3, measureBytes2, sizeof(measureBytes2)),
    };
    uint32_t argCounts[] = {0, 1, 1};
    bool savesLR[] = {true, true, false}; // proc0Body/proc1Body each CALL; proc2Body doesn't
    uint32_t tightArena = 4; // + each measured procedure's own size, below
    for(uint32_t i = 0; i < 3; i++)
    {
        tightArena += measuredHalfwords(measureProcs[i], i, argCounts, 3, savesLR[i]) * 2;
    }

    ProcSource procs[] = {{0, proc0Body, 3}, {1, proc1Body, 4}, {1, proc2Body, 3}};
    uint8_t bytes[48];
    uint32_t maxCallDepth = 2, totalDepth = 2, procCount = 3;
    uint32_t len = makeProgram(maxCallDepth, totalDepth, procs, procCount, bytes, sizeof(bytes));

    // A bigger margin than BOUNDARY_SLACK above: with both the arena and
    // the overall stack budget shrunk to near their real minimum, there's
    // no leftover slack (the totalDepth/maxCallDepth terms that normally
    // provide it) left to absorb translate_proc.cpp's own independent,
    // separately-checked TRANSLATE_BODY_STACK_MARGIN (512) live guard —
    // discovered empirically while tuning this TEST. This isn't trying to
    // find that exact minimum (deliberately out of scope — see this
    // corpus expansion's own plan on avoiding G5-adjacent edge-probing);
    // it just needs enough over BOUNDARY_SLACK to clear that margin too.
    static constexpr uint32_t TIGHT_TEST_SLACK = BOUNDARY_SLACK + 512;
    uint32_t needed = requiredStackBytesFor(procCount, totalDepth, maxCallDepth) + tightArena;
    uint32_t stackLimit = currentSp() - needed - TIGHT_TEST_SLACK;

    ProgramResult r = enterProgramOnStack(nullptr, 0, bytes, len, tightArena, stackLimit, /*interruptReserve=*/0);

    if(r.trapped)
    {
        writeHexTrap(r.value);
    }
    CHECK(!r.trapped);
    CHECK(r.value == 106);
}

/* This image is its own integrator: the strong definition overrides
 * ext_default.cpp's weak one, which is how an extension binds. */
static uint32_t g_extHelperStackBytes = 0;
extern "C" uint32_t extHelperStackBytes() { return g_extHelperStackBytes; }

TEST(AnExtensionsDeclaredHelperStackIsAddedToTheUpFrontBudget)
{
    // Same program, same arena, same stackLimit — the ONLY difference is
    // that an extension declares helper stack. If that declaration doesn't
    // reach requiredStackBytes, both runs behave identically and the static
    // reservation stops being a bound at the one moment it matters: a
    // helper runs at the deepest point of an excursion.
    //
    // The program contains no extension opcodes, so decode is never called;
    // helperStackBytes is consulted regardless.
    const Instr body[] = {CONST(37), bare(Op::RETURN)};
    ProcSource procs[] = {{0, body, 2}};
    uint8_t bytes[32];
    uint32_t maxCallDepth = 0, totalDepth = 1, procCount = 1;
    uint32_t len = makeProgram(maxCallDepth, totalDepth, procs, procCount, bytes, sizeof(bytes));

    uint32_t needed = requiredStackBytesFor(procCount, totalDepth, maxCallDepth) + GENEROUS_ARENA;
    uint32_t stackLimit = currentSp() - needed - BOUNDARY_SLACK;

    ProgramResult without = enterProgramOnStack(nullptr, 0, bytes, len, GENEROUS_ARENA, stackLimit, /*interruptReserve=*/0);
    if(without.trapped)
    {
        writeHexTrap(without.value);
    }
    CHECK(!without.trapped);
    CHECK(without.value == 37);

    // Declaring more than the slack that just made it fit must tip it over.
    g_extHelperStackBytes = BOUNDARY_SLACK * 2;
    ProgramResult with = enterProgramOnStack(nullptr, 0, bytes, len,
        GENEROUS_ARENA, stackLimit, /*interruptReserve=*/0);
    g_extHelperStackBytes = 0;
    CHECK(with.trapped);
    CHECK(with.value == RESOURCE_EXHAUSTED_STACK_BUDGET);
}

int main(void)
{
    // Must run before anything else pushes a single frame — see
    // stack_paint.cpp's own header comment for why (docs/design.md
    // G2/G3/G5's empirical corroboration).
    paintStack();

    initFixtures();
    bool ok = test::TestRunner::runAllTests(&SemihostingOutput::instance);
    ok = reportStackHighWaterMark() && ok;
    semihostingExit(ok ? 0 : 1);
    return 0;
}
