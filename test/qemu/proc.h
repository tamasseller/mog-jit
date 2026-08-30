#ifndef JIT_ARMV6M_TEST_QEMU_PROC_H_
#define JIT_ARMV6M_TEST_QEMU_PROC_H_

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

#endif // JIT_ARMV6M_TEST_QEMU_PROC_H_
