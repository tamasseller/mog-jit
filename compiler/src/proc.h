// jit-armv6m/compiler — the new, FlashProc-like bytecode struct. Not a
// modification of runtime_host.h's FlashProc (that stays untouched, mock
// translator path) — a separate, additive type reached only through
// compile_proc_real.cpp's own g_realProcs table.
//
// body is the procedure's own raw wire bytes (isa-core.md §5), not a
// pre-decoded Instr[] — matching translateProc.ts's own §16 item 16 shift
// (docs/design.md), for the same reason it mattered there: a real target
// never has an already-decoded instruction array lying around for an
// arbitrary-length procedure body to begin with (that's exactly what the
// compact wire encoding exists to avoid holding in memory at all) — the
// only place Instr[] literals still belong is *authoring* a fixture
// conveniently, via encode_instr.h's encodeBody() turning one into these
// same bytes before it ever reaches translateProc.
#ifndef JIT_ARMV6M_COMPILER_PROC_H_
#define JIT_ARMV6M_COMPILER_PROC_H_

#include <cstdint>

namespace jitc {

struct Proc {
    uint32_t argCount;
    const uint8_t *body;
    uint32_t bodyBytes;
};

} // namespace jitc

#endif // JIT_ARMV6M_COMPILER_PROC_H_
