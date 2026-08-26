// A host build has no dispatch/eviction runtime to link against
// (test/host never compiles runtime/*.cpp — Runtime itself is header-only
// and portable, per runtime_internal.h's own doc comment), so it needs
// its own definition of runtimeBail to satisfy that header's contract,
// the same way test_runtime_arena.cpp already supplies its own
// trampolineAddr. An attached Assembler reaching this in a host test
// means arena exhaustion genuinely wasn't recoverable by eviction — a
// real finding the test should see, not something to silently unwind
// past — so this just fails loudly rather than reproducing runtime.S's
// own sp-restore-and-jump escape.
#include "runtime_internal.h"

#include <cstdio>
#include <cstdlib>

#include "Test.h"
#include "Mock.h"

extern "C" void runtimeBail(Runtime *, uint32_t code)
{
    MOCK(runtime)::CALL(runtimeBail).withParam(code);
}
