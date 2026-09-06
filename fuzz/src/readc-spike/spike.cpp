/* fuzz — does SYS_READC work on this QEMU/machine combination?
 *
 * The one open question behind turning the target into a long-lived service
 * (design.md §19's `Executor::cancel` as a watchdog): a persistent guest has
 * to read its next program from somewhere. Semihosting file I/O is already
 * known broken here — SYS_OPEN returns -1 for every path, the ":tt" stdin
 * special case included — while SYS_WRITE0 works. SYS_READC (0x07) is a
 * different code path and takes no argument at all, so it either works or it
 * does not, and twenty lines settle it.
 *
 * Reads until it has six non-zero characters and echoes each as a tagged
 * line.
 *
 * Result, on QEMU 7.2.22 / -M microbit:
 *
 *   - `target=native` alone: SYS_READC blocks forever. Piped stdin never
 *     reaches it, the same way SYS_OPEN never finds a path.
 *   - `-chardev stdio,id=sh0` + `...,chardev=sh0`: it returns. Semihosting
 *     output moves to that chardev too, so stdout rather than stderr.
 *   - It is **non-blocking and lossy**. An empty buffer reads back as 0
 *     rather than waiting, and feeding `ABCDEF` yields `A`, `C`, `E` — every
 *     other byte, with the interleaved SYS_WRITE0 on the same chardev the
 *     likely cause.
 *
 * So the transport exists but is not a byte pipe: anything built on it needs
 * framing and retry, or a chardev each way. See docs/TODO.md.
 */

#include <stdint.h>

#include "semihost.h"

static constexpr uint32_t SYS_READC = 0x07;

static uint32_t readc()
{
    register uint32_t r0 asm("r0") = SYS_READC;
    register uint32_t r1 asm("r1") = 0;
    asm volatile("bkpt 0xAB" : "+r"(r0) : "r"(r1) : "memory");
    return r0;
}

int main(void)
{
    semihostWrite0("spike: SYS_READC\n");
    uint32_t shown = 0;
    for(uint32_t i = 0; i < 200000 && shown < 6; i++)
    {
        const uint32_t c = readc();
        if(c != 0)
        {
            semihostWriteTagged("C:", c);
            shown++;
        }
    }
    semihostWriteTagged("SEEN:", shown);
    semihostExit(0);
}
