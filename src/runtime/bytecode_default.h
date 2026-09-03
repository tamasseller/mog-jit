#ifndef JIT_ARMV6M_RUNTIME_BYTECODE_DEFAULT_H_
#define JIT_ARMV6M_RUNTIME_BYTECODE_DEFAULT_H_

#include "bytecode.h"

/** The mapped accessor's own handle-from-address, for a caller that knows its
 *  program is addressable. Nothing in the core calls it. */
inline BcHandle bcMapped(const void *at)
{
    return (BcHandle)(uintptr_t)at;
}

#endif /* JIT_ARMV6M_RUNTIME_BYTECODE_DEFAULT_H_ */
