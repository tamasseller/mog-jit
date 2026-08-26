// The bytecode-procedure struct compileProc (runtime/compile_proc.cpp)
// builds from the whole-program directory's own ProcSlot (runtime_internal.h)
// for every real call. body is the procedure's own raw wire bytes, not a
// pre-decoded Instr[] — a real target never has an already-decoded
// instruction array lying around for an arbitrary-length procedure body;
// Instr[] literals only exist for authoring a fixture conveniently, via
// encode_instr.h's encodeBody() turning one into these same bytes before it
// ever reaches translateProc.
#ifndef JIT_ARMV6M_COMPILER_PROC_H_
#define JIT_ARMV6M_COMPILER_PROC_H_

#include <cstdint>

namespace jitc
{

struct Proc
{
    uint32_t argCount;
    const uint8_t *body;
    uint32_t bodyBytes;
};

} // namespace jitc

#endif // JIT_ARMV6M_COMPILER_PROC_H_
