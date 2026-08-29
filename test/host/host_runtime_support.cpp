// A host build has no dispatch/eviction runtime to link against
// (test/host never compiles runtime/*.cpp — Runtime itself is header-only
// and portable, per runtime.h's own doc comment), so it needs
// its own definition of runtimeBail to satisfy that header's contract,
// the same way test_runtime_arena.cpp already supplies its own
// trampolineAddr. runtimeBail is declared [[noreturn]] — a caller
// (Assembler::fail(), compiled against that declaration) is entitled to
// assume nothing runs after the call, so this must actually never
// return: MOCK(runtime)::CALL both records the call and, on a mismatch,
// already escapes via TestRunner::failTest's own longjmp; on a match it
// falls through here, so this still needs its own escape via
// resourceErrorEscape (host_runtime_support.h) to honor the contract.
#include "runtime.h"
#include "host_runtime_support.h"

extern "C" void runtimeBail(Runtime *, uint32_t code)
{
    MOCK(runtime)::CALL(runtimeBail).withParam(code);
    longjmp(resourceErrorEscape, 1);
}

jmp_buf resourceErrorEscape;
