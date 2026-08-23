/* Minimal semihosting I/O — extracted from jit-armv6m/test/qemu's own
 * SemihostingOutput (see that file's header for why BKPT 0xAB / native
 * target is the right mechanism). No SemihostingOutput class, no
 * TestRunner: this harness only ever runs one translated procedure and
 * needs to report its 32-bit result back to the host, which a process
 * exit code can't carry (POSIX truncates it to 8 bits) — so the result is
 * printed as a tagged hex line instead, and exit-code-based `TRAP`
 * detection is only used for the halt-on-nothing-else fallback.
 *
 * C++ now (qemu/Makefile's own -fno-exceptions -fno-rtti), not "plain C
 * to dodge vtables/RTTI/operator-delete" the way an earlier draft of this
 * comment put it: none of that cost was ever about the file extension —
 * it's about which *features* get used, and nothing here declares a
 * virtual function, throws, or calls new/delete regardless of which
 * compiler front-end reads it. */

#include <stdint.h>

static inline uint32_t semihosting_call(uint32_t op, void *arg)
{
    register uint32_t r0 asm("r0") = op;
    register void *r1 asm("r1") = arg;
    asm volatile("bkpt 0xAB" : "+r"(r0) : "r"(r1) : "memory");
    return r0;
}

extern "C" void semihosting_write0(const char *s)
{
    semihosting_call(0x04 /* SYS_WRITE0 */, (void *)s);
}

extern "C" void semihosting_exit(int code)
{
    static uint32_t block[2];
    block[0] = 0x20026; /* ADP_Stopped_ApplicationExit */
    block[1] = (uint32_t)code;
    semihosting_call(0x20 /* SYS_EXIT_EXTENDED */, block);
    for(;;) {}
}

/** Prints "<prefix>xxxxxxxx\n" (8 lowercase hex digits). */
static void write_hex_tagged(const char *prefix, uint32_t v)
{
    static char buf[16];
    int i = 0;
    for(; prefix[i]; i++) buf[i] = prefix[i];
    for(int shift = 28; shift >= 0; shift -= 4)
    {
        uint32_t nibble = (v >> shift) & 0xF;
        buf[i++] = nibble < 10 ? ('0' + nibble) : ('a' + nibble - 10);
    }
    buf[i++] = '\n';
    buf[i] = '\0';
    semihosting_write0(buf);
}

/** Prints "RESULT:xxxxxxxx\n" — test/qemu-run.ts's own counterpart parses
 *  exactly this tag out of QEMU's captured stdout. */
extern "C" void write_hex_result(uint32_t v) { write_hex_tagged("RESULT:", v); }

/** Prints "TRAP:xxxxxxxx\n" — test/qemu-run-abi.ts's own counterpart (the
 *  real-ABI harness's landing convention, jit-armv6m/docs/design.md §12)
 *  parses this tag out for the RESOURCE_ERROR/trap path. */
extern "C" void write_hex_trap(uint32_t v) { write_hex_tagged("TRAP:", v); }
