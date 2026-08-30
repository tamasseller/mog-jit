#include "Test.h"
#include "semihosting_output.h"
#include "stack_paint.h"

// Proves the boot + semihosting round-trip works at all — vectors.S reaches
// main(), 1test's registry/runner execute on real Cortex-M-class hardware
// (well, QEMU's lm3s811evb model), and the result makes it back out via
// semihosting. No JIT logic here by design, so it stays green when every
// test_*.cpp fails and says the harness itself is fine.
TEST(Sanity)
{
    CHECK(true);
}

int main(void)
{
    paintStack();

    bool ok = test::TestRunner::runAllTests(&SemihostingOutput::instance);
    ok = reportStackHighWaterMark() && ok;
    semihostingExit(ok ? 0 : 1);
    return 0;
}
