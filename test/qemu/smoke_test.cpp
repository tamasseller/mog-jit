// Proves the boot + semihosting round-trip works at all — vectors.s reaches
// main(), 1test's registry/runner execute on real Cortex-M-class hardware
// (well, QEMU's lm3s811evb model), and the result makes it back out via
// semihosting. No JIT logic here by design (see docs/jit-armv6m.md §16 and
// the scaffolding plan) — this is purely a "does the harness work" check.

#include "Test.h"

TEST(Sanity)
{
    CHECK(true);
}
