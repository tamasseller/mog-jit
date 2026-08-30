#include "ext.h"
#include "assembler.h"
#include "runtime.h"

extern "C" __attribute__((weak)) uint32_t extDecode(const uint8_t *, uint32_t, uint32_t, uint32_t *)
{
    return 0;
}

extern "C" __attribute__((weak)) void extEmit(jitc::Assembler &a, const ExtSite &)
{
    runtimeBail(&a.runtime, RESOURCE_PROGRAM_EXT_UNSUPPORTED);
}

extern "C" __attribute__((weak)) uint32_t extHelperStackBytes()
{
    return 0;
}
