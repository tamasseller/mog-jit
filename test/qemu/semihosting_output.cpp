#include "semihosting_output.h"

namespace
{
    constexpr uint32_t sysWrite0 = 0x04;
    constexpr uint32_t sysExitExtended = 0x20;
    constexpr uint32_t adpStoppedApplicationExit = 0x20026;

    inline uint32_t semihostingCall(uint32_t op, void *arg)
    {
        register uint32_t r0 asm("r0") = op;
        register void *r1 asm("r1") = arg;
        asm volatile("bkpt 0xAB" : "+r"(r0) : "r"(r1) : "memory");
        return r0;
    }

    // No libc console/printf on this bare image — just enough decimal
    // formatting for the final normal/failure counts. No division, not
    // even by the constant 10: Cortex-M0 has no divide instruction and no
    // 32x32->64 multiply either, so even `v % 10` on a compile-time
    // literal still calls libgcc's __aeabi_uidiv — repeated subtraction
    // against a fixed set of places avoids that entirely. Counts here are
    // always small (fixture/scenario counts), so four decimal digits of
    // headroom is plenty.
    void writeUint(uint32_t v)
    {
        static const uint32_t PLACES[] = {1000, 100, 10, 1};
        char buf[5];
        uint32_t n = 0;
        bool started = false;
        for(uint32_t place : PLACES)
        {
            uint32_t digit = 0;
            while(v >= place)
            {
                v -= place;
                digit++;
            }
            if(digit != 0 || started || place == 1)
            {
                buf[n++] = (char)('0' + digit);
                started = true;
            }
        }
        buf[n] = '\0';
        semihostingWrite0(buf);
    }

    // "<prefix>xxxxxxxx\n" (8 lowercase hex digits) — shared by
    // writeHexResult/writeHexTrap below.
    void writeHexTagged(const char *prefix, uint32_t v)
    {
        char buf[16];
        int i = 0;
        for(; prefix[i]; i++)
        {
            buf[i] = prefix[i];
        }
        for(int shift = 28; shift >= 0; shift -= 4)
        {
            uint32_t nibble = (v >> shift) & 0xF;
            buf[i++] = nibble < 10 ? (char)('0' + nibble) : (char)('a' + nibble - 10);
        }
        buf[i++] = '\n';
        buf[i] = '\0';
        semihostingWrite0(buf);
    }
}

void writeHexResult(uint32_t v)
{
    writeHexTagged("RESULT:", v);
}

void writeHexTrap(uint32_t v)
{
    writeHexTagged("TRAP:", v);
}

void semihostingWrite0(const char *s)
{
    semihostingCall(sysWrite0, (void *)s);
}

[[noreturn]] void semihostingExit(int code)
{
    static uint32_t block[2];
    block[0] = adpStoppedApplicationExit;
    block[1] = (uint32_t)code;
    semihostingCall(sysExitExtended, block);

    for(;;)
    {
        // Should never be reached — a host that ignored the exit request
        // still shouldn't fall through into whatever follows in memory.
    }
}

SemihostingOutput SemihostingOutput::instance;

void SemihostingOutput::reportProgress()
{
    semihostingWrite0(".");
}

void SemihostingOutput::reportTestFailure(const char *testName, const char *sourceInfo,
                                           const char *failureSourceInfo, const char *text)
{
    semihostingWrite0("\nFAIL ");
    semihostingWrite0(testName);
    semihostingWrite0(" (");
    semihostingWrite0(sourceInfo);
    semihostingWrite0(") at ");
    semihostingWrite0(failureSourceInfo);
    if(text)
    {
        semihostingWrite0(": ");
        semihostingWrite0(text);
    }
    semihostingWrite0("\n");
}

void SemihostingOutput::reportFinal(uint32_t normal, uint32_t failure)
{
    if(failure)
    {
        semihostingWrite0("\nERROR: ");
        writeUint(failure);
        semihostingWrite0(" of ");
        writeUint(normal);
        semihostingWrite0(" tests failed!\n");
    }
    else if(normal)
    {
        semihostingWrite0("\nOK: all ");
        writeUint(normal);
        semihostingWrite0(" tests passed.\n");
    }
    else
    {
        semihostingWrite0("\nNo tests registered to run!\n");
    }
}
