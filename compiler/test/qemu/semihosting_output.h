// test::TestOutput sink for 1test's TestRunner (vendor/1test), printing
// over ARM semihosting (SYS_WRITE0) — this target is bare-metal
// (-nostartfiles -specs=nosys.specs), so PrintfOutput's <iostream> isn't
// available. Mirrors PrintfOutput's own shape, just over a different sink.
#ifndef JIT_ARMV6M_COMPILER_TEST_QEMU_SEMIHOSTING_OUTPUT_H_
#define JIT_ARMV6M_COMPILER_TEST_QEMU_SEMIHOSTING_OUTPUT_H_

#include "TestOutput.h"

class SemihostingOutput: public test::TestOutput
{
protected:
    // TestRunner always calls the named overload below; this one exists
    // only to satisfy TestOutput's own pure virtual.
    void reportProgress() override;
    void reportProgress(const char *testName) override;
    void reportTestFailure(const char *testName, const char *sourceInfo, const char *failureSourceInfo, const char *text) override;
    void reportFinal(uint32_t normal, uint32_t failure) override;

public:
    static SemihostingOutput instance;
};

#endif // JIT_ARMV6M_COMPILER_TEST_QEMU_SEMIHOSTING_OUTPUT_H_
