/* jit-armv6m/fuzz/qemu_exec — the execution oracle harness.cpp's own
 * TODO(execute) has been waiting for.
 *
 * Everything the host-side fuzzer can check is "did the translator crash or
 * bail cleanly". It cannot check the one thing a JIT is actually for: that
 * the code it emitted *computes the right answer*. A miscompilation — a
 * wrong fold, a clobbered window register, an off-by-one spill offset, a
 * merge point reading a register the other edge never wrote — is completely
 * invisible to it, because nothing there ever runs the Thumb.
 *
 * This is that missing half, and it needs no new emulator: test/qemu
 * already runs this exact translator plus the real, unmodified runtime/ on
 * qemu-system-arm. The only thing it doesn't do is take its programs from
 * outside the image. So this runner reads a whole *batch* of fuzzer
 * programs and runs each one through the real enterProgramSplit, printing
 * one result line each — one QEMU boot per batch rather than per program,
 * which is what makes the emulator affordable here.
 *
 * qemu_exec.ts drives it and does the comparison against @ppl/machine's
 * reference VM.
 *
 * How the batch gets in: `-device loader,file=<batch>,addr=BATCH_ADDR`
 * writes it straight into guest flash before reset, and this runner reads
 * it from there. Semihosting file I/O would have been the obvious route and
 * was tried first — SYS_OPEN returns -1 in this QEMU/machine combination
 * for every path, the ":tt" stdin special case included, while SYS_WRITE0
 * works fine. The loader route needs no semihosting at all for input, and
 * no protocol that could go wrong; the cost is that a batch has to fit in
 * flash above the image, so qemu_exec.ts splits large corpora across boots.
 *
 * Batch format at BATCH_ADDR (little-endian, as qemu_exec.ts writes it):
 *     u32 magic == BATCH_MAGIC
 *     u32 count
 *     count × ( u32 length, length bytes of one whole program envelope )
 *
 * Output, one line per program, in order:
 *     R:xxxxxxxx   normal return, the entry procedure's own result
 *     T:xxxxxxxx   bytecode TRAP, the trap code
 *     E:xxxxxxxx   RESOURCE_ERROR (arena/stack budget) — a legitimate
 *                  outcome, not comparable against the reference VM
 *     X:xxxxxxxx   rejected before running (length past PROGRAM_MAX)
 * then
 *     DONE:xxxxxxxx  how many programs were run
 *
 * NDEBUG is on (the shared bare-metal constraint — a firing assert() would
 * need newlib's fprintf path and a heap this design has no room for): this
 * image is emphatically not the crash-finder, it is the answer-checker. The
 * host fuzzer, built with real ASan/UBSan and real asserts, remains the
 * crash-finder.
 */

#include <stdint.h>

#include "semihost.h"
#include "runtime_host.h"

/* Exactly linker.ld's own rom ORIGIN+LENGTH, and BATCH_LIMIT is the rest of
 * this model's flash, whose end was measured at 0xA000 rather than assumed.
 * Placing the batch immediately above the rom region is what makes the
 * linker itself guarantee the two never overlap — an image that outgrew its
 * region would fail to link rather than quietly run over the batch. Flash,
 * never RAM: vectors.S's own .bss zeroing would erase it. BATCH_LIMIT is
 * therefore the hard per-boot batch ceiling, and qemu_exec.ts chunks to
 * it. */
static constexpr uint32_t BATCH_ADDR = 0x00004000u;
static constexpr uint32_t BATCH_LIMIT = 0x00006000u;
static constexpr uint32_t BATCH_MAGIC = 0x50504C42u; /* "PPLB" */

/* Programs are run *in place, out of flash* — Runtime::init only ever reads
 * the body bytes (its ProcSlot::bodyPtr is a read-only cursor the
 * translator decodes from), so there is no reason to copy them into RAM
 * first. This runner used to, and the 1KB buffer that took was the real
 * ceiling on how much of a fuzz corpus the execution oracle could reach at
 * all: it skipped roughly four inputs in five for size, and specifically
 * every one of the large programs written to straddle the translator's own
 * compiled-size guards. Now the only ceiling is the batch window itself.
 *
 * PROGRAM_MAX matches harness.cpp's own input ceiling, so the two halves
 * accept the same programs; it is a framing sanity bound here, not a memory
 * one.
 *
 * RAM is 8KB on this model (test/qemu passes `-m 8k` for the same reason)
 * and has to hold the code arena, Runtime, the operand stack and the
 * translator's own recursive C stack. The arena gets 5KB of it, leaving
 * about 3KB of stack against a typical program's own requirement
 * (`requiredStackBytes`: Runtime, the operand stack, and
 * TRANSLATOR_ENTRY_WORST_CASE_BYTES's 444 — roughly 600 to 1500 bytes) plus
 * the translator's own 168-bytes-per-nesting-level recursion.
 *
 * Sized from measurement, not taste: at 3KB, 84% of a real fuzz corpus came
 * back RESOURCE_ERROR once programs up to 4KB were in range, so the sweep
 * spent nearly all its time on programs whose emitted code it never got to
 * check. Running out is still a legitimate outcome the comparison skips
 * rather than a failure — there just shouldn't be that much of it. */
static constexpr uint32_t PROGRAM_MAX = 4096;
static constexpr uint32_t CODE_ARENA_BYTES = 3072;

static uint8_t g_codeArena[CODE_ARENA_BYTES] __attribute__((aligned(4)));

/* Flash is byte-addressable here, but the batch's own u32 fields have no
 * alignment guarantee relative to a preceding program's length, so they are
 * read byte by byte rather than through a u32 load — an unaligned word load
 * on a Cortex-M0 faults. */
static uint32_t readU32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int main(void)
{
    const uint8_t *cursor = (const uint8_t *)(uintptr_t)BATCH_ADDR;
    const uint8_t *end = cursor + BATCH_LIMIT;

    if(readU32(cursor) != BATCH_MAGIC)
    {
        /* Loud rather than "run whatever is at that address": an unloaded
         * or mis-addressed batch would otherwise be interpreted as
         * programs, and every result it printed would be meaningless. */
        semihostWrite0("FATAL: no batch at BATCH_ADDR (magic mismatch)\n");
        semihostExit(2);
    }
    cursor += 4;

    const uint32_t count = readU32(cursor);
    cursor += 4;

    uint32_t ran = 0;
    for(uint32_t i = 0; i < count; i++)
    {
        if(cursor + 4 > end) break;
        const uint32_t len = readU32(cursor);
        cursor += 4;

        if(len == 0 || len > PROGRAM_MAX || len > (uint32_t)(end - cursor))
        {
            semihostWriteTagged("X:", len);
            break; /* framing is unusable from here on, don't walk off flash */
        }

        const uint8_t *programBytes = cursor;
        cursor += len;

        /* The split variant, not enterProgramOnStack: its arena is a
         * distinct region rather than the same memory the translator's own
         * recursion runs on, so a deep compilation cannot quietly corrupt
         * the arena it is writing into, and a RESOURCE_ERROR here means
         * what it says. stackLimit is the floor this excursion may reach
         * below the current sp; everything under g_codeArena is other
         * .bss, so anchoring it at the arena's own top keeps the two from
         * ever meeting.
         *
         * interruptReserve 0: no interrupts are enabled in this image. */
        ProgramResult r = enterProgramSplit(
            /*argIn=*/0,
            programBytes, len,
            (uint32_t)(uintptr_t)g_codeArena, CODE_ARENA_BYTES,
            /*stackLimit=*/(uint32_t)(uintptr_t)(g_codeArena + CODE_ARENA_BYTES),
            /*interruptReserve=*/0);

        ran++;

        /* Three fully distinguishable outcomes, straight off
         * ProgramResult::trapped's own LANDING_* tag (runtime_host.h) —
         * nothing is encoded in `value`'s bits, so a program may return
         * any uint32_t and trap with any code without the two aliasing. */
        if(r.trapped == LANDING_TRAP)
        {
            semihostWriteTagged("T:", r.value);
        }
        else if(r.trapped)
        {
            semihostWriteTagged("E:", r.value);
        }
        else
        {
            semihostWriteTagged("R:", r.value);
        }
    }

    semihostWriteTagged("DONE:", ran);
    semihostExit(0);
}
