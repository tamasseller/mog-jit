#include "semihosting_output.h"

namespace
{
    constexpr uint32_t SYS_WRITE0 = 0x04;
    constexpr uint32_t SYS_EXIT_EXTENDED = 0x20;
    constexpr uint32_t ADP_STOPPED_APPLICATION_EXIT = 0x20026;

    inline uint32_t semihostingCall(uint32_t op, void *arg)
    {
        register uint32_t r0 asm("r0") = op;
        register void *r1 asm("r1") = arg;
        asm volatile("bkpt 0xAB" : "+r"(r0) : "r"(r1) : "memory");
        return r0;
    }

    // No libc console/printf on this bare image — just enough decimal
    // formatting for the progress dots and final normal/failure counts.
    void writeUint(uint32_t v)
    {
        char buf[11];
        char *p = buf + sizeof(buf);
        *--p = '\0';

        do
        {
            *--p = '0' + (v % 10);
            v /= 10;
        } while (v != 0);

        semihosting_write0(p);
    }
}

void semihosting_write0(const char *s)
{
    semihostingCall(SYS_WRITE0, (void *)s);
}

[[noreturn]] void semihosting_exit(int code)
{
    static uint32_t block[2];
    block[0] = ADP_STOPPED_APPLICATION_EXIT;
    block[1] = (uint32_t)code;
    semihostingCall(SYS_EXIT_EXTENDED, block);

    for (;;)
    {
        // Should never be reached — a host that ignored the exit request
        // still shouldn't fall through into whatever follows in memory.
    }
}

SemihostingOutput SemihostingOutput::instance;

void SemihostingOutput::reportProgress()
{
    semihosting_write0(".");
}

void SemihostingOutput::reportTestFailure(const char *testName, const char *sourceInfo,
                                           const char *failureSourceInfo, const char *text)
{
    semihosting_write0("\nFAIL ");
    semihosting_write0(testName);
    semihosting_write0(" (");
    semihosting_write0(sourceInfo);
    semihosting_write0(") at ");
    semihosting_write0(failureSourceInfo);
    if (text)
    {
        semihosting_write0(": ");
        semihosting_write0(text);
    }
    semihosting_write0("\n");
}

void SemihostingOutput::reportFinal(uint32_t normal, uint32_t failure)
{
    if (failure)
    {
        semihosting_write0("\nERROR: ");
        writeUint(failure);
        semihosting_write0(" of ");
        writeUint(normal);
        semihosting_write0(" tests failed!\n");
    }
    else if (normal)
    {
        semihosting_write0("\nOK: all ");
        writeUint(normal);
        semihosting_write0(" tests passed.\n");
    }
    else
    {
        semihosting_write0("\nNo tests registered to run!\n");
    }
}
