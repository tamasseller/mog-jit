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
        } while(v != 0);

        semihostingWrite0(p);
    }
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
