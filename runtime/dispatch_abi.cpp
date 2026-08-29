#include "dispatch_abi.h"

extern const uint32_t trampolineAddr = (uint32_t)(uintptr_t)translatorTrampoline;

extern const uint32_t helperVec[10] = {
    (uint32_t)(uintptr_t)callHelper,
    (uint32_t)(uintptr_t)returnHelperFromLr,
    (uint32_t)(uintptr_t)returnHelperFromStack,
    (uint32_t)(uintptr_t)returnHelperTail,
    (uint32_t)(uintptr_t)clzHelper,
    (uint32_t)(uintptr_t)revbitsHelper,
    (uint32_t)(uintptr_t)brTableJumpHelper,
    (uint32_t)(uintptr_t)returnHelperFromStackReclaim,
    (uint32_t)(uintptr_t)trapHelper,
    (uint32_t)(uintptr_t)extThunkHelper,
};

extern "C" void runtimeBail(Runtime *runtime, uint32_t trapCode)
{
    register uint32_t trapCodeReg asm("r0") = trapCode;
    register uint32_t tagReg asm("r2") = LANDING_RESOURCE_ERROR;
    register uint32_t landingReg asm("r3") = runtime->sentinelLandingAddress();
    register uint32_t savedSpReg asm("r1") = runtime->savedSp;
    asm volatile("mov sp, r1\n\tbx r3" : : "r"(trapCodeReg), "r"(savedSpReg), "r"(tagReg), "r"(landingReg));
    __builtin_unreachable();
}
