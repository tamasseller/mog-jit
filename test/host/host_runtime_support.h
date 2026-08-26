// The escape a host test unwinds through when the code under test reaches
// Assembler::fail() -> runtimeBail() (see runtime_internal.h's own doc
// comment) -- runtimeBail is [[noreturn]], and host_runtime_support.cpp's
// mock honors that by longjmp-ing here instead of returning, exactly the
// way 1test's own CHECK() failures already unwind a test. A call site
// reached only through fail() must never resume normally, so pair every
// MOCK(runtime)::EXPECT(runtimeBail) with this rather than a bare call.
#ifndef HOST_RUNTIME_SUPPORT_H_
#define HOST_RUNTIME_SUPPORT_H_

#include "setjmp.h" // Intentionally not <setjmp.h> !

#include "Test.h"

extern jmp_buf resourceErrorEscape;

#define EXPECT_RESOURCE_ERROR(action)                                        \
    do                                                                       \
    {                                                                        \
        if(!setjmp(resourceErrorEscape))                                     \
        {                                                                    \
            action;                                                         \
            CHECK(false); /* GCOV_EXCL_LINE — unreachable: runtimeBail escapes first */ \
        }                                                                     \
    } while(0)

#endif // HOST_RUNTIME_SUPPORT_H_
