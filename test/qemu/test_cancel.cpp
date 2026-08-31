// Executor::cancel — ending a running excursion from an exception handler,
// the way an application cancels a user-supplied program that will not stop
// on its own. SysTick stands in for whatever the application's own timeout,
// watchdog or communication loop is.

#include <cstdint>

#include "instr.h"
#include "encode_instr.h"
#include "executor.h"
#include "dispatch_abi.h"
#include "run_program.h"
#include "Test.h"

using namespace jitc;

namespace
{

#define SYST_CSR (*(volatile uint32_t *)0xe000e010u)
#define SYST_RVR (*(volatile uint32_t *)0xe000e014u)
#define SYST_CVR (*(volatile uint32_t *)0xe000e018u)

constexpr uint32_t CANCEL_CODE = 0x5a5a;

// Covers the deepest of these programs with room to spare — an over-generous
// envelope only enlarges run()'s own up-front reservation.
constexpr uint32_t MAX_CALL_DEPTH = 2;
constexpr uint32_t TOTAL_DEPTH = 8;

// The handler's own frame on top of what exception entry pushes.
constexpr uint32_t CANCEL_INTERRUPT_RESERVE = ARMV6M_EXCEPTION_FRAME_BYTES + 32;

uint8_t g_arena[ARENA_BYTES];

Executor *g_target;
volatile uint32_t g_cancels;
volatile uint32_t g_interruptedPc;

void systickOn(uint32_t reload)
{
    SYST_RVR = reload;
    SYST_CVR = 0;
    SYST_CSR = 7; // core clock, tick interrupt, enable
}

void systickOff()
{
    SYST_CSR = 0;
}

bool inArena(uint32_t pc)
{
    return pc >= (uint32_t)(uintptr_t)g_arena && pc < (uint32_t)(uintptr_t)g_arena + ARENA_BYTES;
}

/* An endless LOOP, reached two calls down with an out-of-window argument, so
 * the sp it is interrupted at is genuinely below the one the landing needs —
 * without the depth, the trampoline's own sp restore would be a no-op and
 * nothing here would test it. The condition block is a bare non-zero
 * constant, so nothing but a cancellation ends the loop. */
const Instr endlessProc0[] = {
    CONST(1), PUSH(),
    CONST(2), PUSH(),
    CONST(3), PUSH(),
    CONST(4), PUSH(),
    CONST(5),
    call(1),
    bare(Op::RETURN),
};
const Instr endlessProc1[] = {LOAD(0), call(2), bare(Op::RETURN)};
const Instr endlessProc2[] = {
    bare(Op::LOOP),
        CONST(1),
    bare(Op::BLOCK_END),
        CONST(0),
    bare(Op::BLOCK_END),
    CONST(7), bare(Op::RETURN),
};

#define ENDLESS_PROCS \
    ProcSource endless[] = { \
        PROC(0, endlessProc0), PROC(5, endlessProc1), PROC(1, endlessProc2)}

const Instr constProc0[] = {CONST(11), bare(Op::RETURN)};

} // namespace

/* What an application writes: recover the frame the exception was entered on
 * and hand it to the Executor that owns the excursion. */
extern "C" void sysTickCancel(uint32_t *frame)
{
    g_interruptedPc = frame[6];

    if(g_target && g_target->cancel((uint32_t)(uintptr_t)frame, CANCEL_CODE))
    {
        g_cancels++;
        systickOff();
    }
}

/* EXC_RETURN bit 2 says which stack the interrupted context was on, and the
 * frame sits at the top of that one. Naked, so nothing is pushed before the
 * read. */
extern "C" __attribute__((naked)) void sysTickHandler()
{
    asm volatile(
        "movs r0, #4\n\t"
        "mov  r1, lr\n\t"
        "tst  r1, r0\n\t"
        "mrs  r0, msp\n\t"
        "beq  1f\n\t"
        "mrs  r0, psp\n\t"
        "1:\n\t"
        "push {lr}\n\t"
        "bl   sysTickCancel\n\t"
        "pop  {pc}\n\t");
}

namespace
{

Executor makeExecutor()
{
    return Executor::split((uint32_t)(uintptr_t)g_arena, ARENA_BYTES,
        (uint32_t)(uintptr_t)&__bss_end + STACK_SLACK_ABOVE_BSS, CANCEL_INTERRUPT_RESERVE);
}

ProgramResult runWith(Executor &ex, const ProcSource *procs, uint32_t procCount, uint32_t *args)
{
    uint8_t bytes[PROGRAM_CAPACITY];
    const uint32_t len = encodeJitProgram(MAX_CALL_DEPTH, TOTAL_DEPTH, procs, procCount, bytes, sizeof(bytes));

    return ex.run(bytes, len, args, procs[0].argCount);
}

// Long enough that translation is finished and the tick lands in compiled
// code; the loop never ends, so nothing is lost by being generous.
constexpr uint32_t RELOAD_INSIDE_LOOP = 50000;

} // namespace

TEST(CancelEndsAnOtherwiseEndlessLoop)
{
    ENDLESS_PROCS;

    Executor ex = makeExecutor();
    g_target = &ex;
    g_cancels = 0;
    systickOn(RELOAD_INSIDE_LOOP);

    ProgramResult r = runWith(ex, endless, 3, nullptr);

    systickOff();
    g_target = nullptr;

    CHECK(g_cancels == 1);
    CHECK(r.trapped == LANDING_CANCELLED);
    CHECK(r.value == CANCEL_CODE);
}

TEST(CancelInterruptsTheCompiledCodeItself)
{
    ENDLESS_PROCS;

    Executor ex = makeExecutor();
    g_target = &ex;
    g_interruptedPc = 0;
    systickOn(RELOAD_INSIDE_LOOP);

    runWith(ex, endless, 3, nullptr);

    systickOff();
    g_target = nullptr;

    CHECK(inArena(g_interruptedPc)); // the loop, not the translator
}

TEST(AnExecutorRunsProgramsAgainAfterACancellation)
{
    // The arena and the whole Runtime are run()'s own frame, and the landing
    // returns through it, so a cancellation leaves nothing behind.
    ENDLESS_PROCS;
    ProcSource plain[] = {PROC(1, constProc0)};
    uint32_t args[] = {0};

    Executor ex = makeExecutor();
    g_target = &ex;
    systickOn(RELOAD_INSIDE_LOOP);

    ProgramResult cancelled = runWith(ex, endless, 3, nullptr);

    systickOff();
    g_target = nullptr;

    CHECK(cancelled.trapped == LANDING_CANCELLED);

    ProgramResult after = runWith(ex, plain, 1, args);

    CHECK(after.trapped == LANDING_SUCCESS);
    CHECK(after.value == 11);
}

TEST(CancelIsRefusedWhenNothingIsRunning)
{
    uint32_t frame[8] = {0};
    Executor ex = makeExecutor();

    CHECK(!ex.cancel((uint32_t)(uintptr_t)frame, CANCEL_CODE));
    CHECK(frame[6] == 0); // the frame is left alone
}

TEST(CancelIsRefusedOnceTheProgramHasReturned)
{
    /* Either guard alone satisfies this: run() clears `live`, and .Lresume
     * clears the landing sp. The second exists for the window between them,
     * which no single-threaded test can provoke. */
    ENDLESS_PROCS;

    Executor ex = makeExecutor();
    g_target = &ex;
    systickOn(RELOAD_INSIDE_LOOP);

    runWith(ex, endless, 3, nullptr);

    systickOff();
    g_target = nullptr;

    uint32_t frame[8] = {0};
    CHECK(!ex.cancel((uint32_t)(uintptr_t)frame, CANCEL_CODE));
}
