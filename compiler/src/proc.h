// jit-armv6m/compiler — the new, FlashProc-like bytecode struct. Not a
// modification of runtime_host.h's FlashProc (that stays untouched, mock
// translator path) — a separate, additive type reached only through
// compile_proc_real.cpp's own g_realProcs table.
#ifndef JIT_ARMV6M_COMPILER_PROC_H_
#define JIT_ARMV6M_COMPILER_PROC_H_

#include <cstdint>
#include "instr.h"

namespace jitc {

struct Proc {
    uint32_t argCount;
    const Instr *body;
    uint32_t bodyCount;
};

} // namespace jitc

#endif // JIT_ARMV6M_COMPILER_PROC_H_
