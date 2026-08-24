#include "semihosting_output.h"

extern "C" void semihostingWrite0(const char *s);

namespace
{

// Decimal, no leading zeros beyond a bare "0".
void writeDec(uint32_t v)
{
    // No division, not even by the constant 10: Cortex-M0 has no divide
    // instruction, so unlike the decode table this file's own writeDec
    // avoided libgcc calls the hard way rather than not needing them at
    // all — division by a runtime value versus a compile-time constant
    // makes no difference to a target with no divider in either case.
    // Values here are always small (fixture/scenario counts), so four
    // decimal digits of headroom by repeated subtraction is plenty.
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

} // namespace

SemihostingOutput SemihostingOutput::instance;

void SemihostingOutput::reportProgress()
{
    reportProgress(nullptr);
}

void SemihostingOutput::reportProgress(const char *testName)
{
    semihostingWrite0("completed: ");
    semihostingWrite0(testName ? testName : "?");
    semihostingWrite0("\n");
}

void SemihostingOutput::reportTestFailure(const char *testName, const char *sourceInfo, const char *failureSourceInfo, const char *text)
{
    semihostingWrite0("\nTest '");
    semihostingWrite0(testName);
    semihostingWrite0("' (");
    semihostingWrite0(sourceInfo);
    semihostingWrite0(")\n\n    failed at ");
    semihostingWrite0(failureSourceInfo);
    if(text)
    {
        semihostingWrite0(": ");
        semihostingWrite0(text);
    }
    semihostingWrite0("\n\n");
}

void SemihostingOutput::reportFinal(uint32_t normal, uint32_t failure)
{
    semihostingWrite0("\n\n");
    if(failure)
    {
        semihostingWrite0("ERROR: ");
        writeDec(failure);
        semihostingWrite0(" of ");
        writeDec(normal);
        semihostingWrite0(" tests failed !");
    }
    else if(normal)
    {
        semihostingWrite0("OK: all ");
        writeDec(normal);
        semihostingWrite0(" tests have been ran successfully.");
    }
    else
    {
        semihostingWrite0("No tests registered to run !"); // GCOV_EXCL_LINE — this build always registers at least one TEST
    }
    semihostingWrite0("\n\n");
}
