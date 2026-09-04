#include "ext.h"
#include "assembler.h"
#include "runtime.h"

extern "C" __attribute__((weak)) bool extDescribe(uint8_t, BcReader &, uint32_t *)
{
    return false;
}

extern "C" __attribute__((weak)) void extEmit(ExtSite &site)
{
    runtimeBail(&site.a.runtime, RESOURCE_PROGRAM_EXT_UNSUPPORTED);
}

extern "C" __attribute__((weak)) uint32_t extHelperStackBytes()
{
    return 0;
}
