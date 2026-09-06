#include "ext.h"
#include "assembler.h"
#include "runtime.h"

#include <cassert>

extern "C" __attribute__((weak)) bool extDescribe(uint8_t, BcReader &, uint32_t *)
{
    return false;
}

extern "C" __attribute__((weak)) void extEmit(ExtSite &site)
{
    // Unreachable in a consistent build: this extDescribe answers no opcode,
    // so proc_scan rejects every EXT before translation reaches here. Only a
    // half-overridden weak pair gets this far.
    (void)site;
    assert(false); // GCOV_EXCL_LINE
}

extern "C" __attribute__((weak)) uint32_t extHelperStackBytes()
{
    return 0;
}
