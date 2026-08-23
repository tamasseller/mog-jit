#ifndef JIT_ARMV6M_TEST_QEMU_SEMIHOSTING_OUTPUT_H_
#define JIT_ARMV6M_TEST_QEMU_SEMIHOSTING_OUTPUT_H_

#include "TestOutput.h"
#include <cstdint>

// ARM semihosting (BKPT 0xAB — the M-profile trap encoding, distinct from
// the SVC-based encoding non-M-profile targets use) is what lets a QEMU
// guest with no OS/UART driver still print text and report a real process
// exit code back to the host.

void semihostingWrite0(const char *s);

// Exits the emulator process itself with `code` as qemu-system-arm's own
// exit status, via SYS_EXIT_EXTENDED (ADP_Stopped_ApplicationExit + a real
// subcode) rather than plain SYS_EXIT, which only distinguishes "graceful"
// vs "not" and can't carry an arbitrary pass/fail code.
[[noreturn]] void semihostingExit(int code);

class SemihostingOutput : public test::TestOutput
{
protected:
    virtual void reportProgress() override;
    virtual void reportTestFailure(const char *testName, const char *sourceInfo,
                                    const char *failureSourceInfo, const char *text) override;
    virtual void reportFinal(uint32_t normal, uint32_t failure) override;
    virtual inline ~SemihostingOutput()
    {
    }

public:
    static SemihostingOutput instance;
};

#endif /* JIT_ARMV6M_TEST_QEMU_SEMIHOSTING_OUTPUT_H_ */
