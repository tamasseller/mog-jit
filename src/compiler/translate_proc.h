// The top-level per-procedure driver — decodes one procedure's wire bytes
// and emits Thumb code via abi_strategy.h's real-ABI sequences.
// Everything reaches the outside world through Runtime: slot(procIdx)
// supplies argCount/bodyHandle/bodyBytes/needsLRSave, and compilation always
// goes through an Assembler attached to r. A host test or QEMU
// pre-measurement gets the same path by handing in a throwaway Runtime
// with only the facts translateProc reads filled in.
#ifndef JIT_ARMV6M_COMPILER_TRANSLATE_PROC_H_
#define JIT_ARMV6M_COMPILER_TRANSLATE_PROC_H_

#include <cstdint>
#include "assembler.h"

extern "C" uint32_t translateProc(uint32_t procIdx, Runtime& r, uint32_t lruTick);

#endif // JIT_ARMV6M_COMPILER_TRANSLATE_PROC_H_
