/* Minimal semihosting I/O for the real-ABI QEMU harness, which only ever
 * runs one translated procedure and needs to report its 32-bit result back
 * to the host — a process exit code can't carry that (POSIX truncates it
 * to 8 bits), so the result is printed as a tagged hex line instead, and
 * exit-code-based TRAP detection is only used for the halt-on-nothing-else
 * fallback. */

#include <stdint.h>

static inline uint32_t semihostingCall(uint32_t op, void *arg)
{
    register uint32_t r0 asm("r0") = op;
    register void *r1 asm("r1") = arg;
    asm volatile("bkpt 0xAB" : "+r"(r0) : "r"(r1) : "memory");
    return r0;
}

extern "C" void semihostingWrite0(const char *s)
{
    semihostingCall(0x04 /* SYS_WRITE0 */, (void *)s);
}

extern "C" void semihostingExit(int code)
{
    static uint32_t block[2];
    block[0] = 0x20026; /* ADP_Stopped_ApplicationExit */
    block[1] = (uint32_t)code;
    semihostingCall(0x20 /* SYS_EXIT_EXTENDED */, block);
    for(;;)
    {
    }
}

/* Prints "<prefix>xxxxxxxx\n" (8 lowercase hex digits). */
static void writeHexTagged(const char *prefix, uint32_t v)
{
    static char buf[16];
    int i = 0;
    for(; prefix[i]; i++)
    {
        buf[i] = prefix[i];
    }
    for(int shift = 28; shift >= 0; shift -= 4)
    {
        uint32_t nibble = (v >> shift) & 0xF;
        buf[i++] = nibble < 10 ? ('0' + nibble) : ('a' + nibble - 10);
    }
    buf[i++] = '\n';
    buf[i] = '\0';
    semihostingWrite0(buf);
}

/* Prints "RESULT:xxxxxxxx\n" — the QEMU test harness's own runner parses
 * exactly this tag out of QEMU's captured stdout. */
extern "C" void writeHexResult(uint32_t v)
{
    writeHexTagged("RESULT:", v);
}

/* Prints "TRAP:xxxxxxxx\n" — the real-ABI harness's landing convention for
 * the RESOURCE_ERROR/trap path. */
extern "C" void writeHexTrap(uint32_t v)
{
    writeHexTagged("TRAP:", v);
}
