#include "dispatch_abi.h"
#include "registers.h"
#include "abi_strategy.h"

extern const uint32_t trampolineAddr = (uint32_t)(uintptr_t)translatorTrampoline;

extern "C" const HelperVec helperVec = {
    .call                   = (uint32_t)(uintptr_t)callHelper,
    .returnFromLr           = (uint32_t)(uintptr_t)returnHelperFromLr,
    .returnFromStack        = (uint32_t)(uintptr_t)returnHelperFromStack,
    .returnTail             = (uint32_t)(uintptr_t)returnHelperTail,
    .clz                    = (uint32_t)(uintptr_t)clzHelper,
    .revbits                = (uint32_t)(uintptr_t)revbitsHelper,
    .brTableJump            = (uint32_t)(uintptr_t)brTableJumpHelper,
    .returnFromStackReclaim = (uint32_t)(uintptr_t)returnHelperFromStackReclaim,
    .trap                   = (uint32_t)(uintptr_t)trapHelper,
    .extThunk               = (uint32_t)(uintptr_t)extThunkHelper,
};

static_assert(jitc::packRecord((uint32_t)-1, 0) == CALL_RECORD_BOOT,
    "runtime.S's own boot record must be what packRecord would have produced");

static_assert(sizeof(ProcSlot) == DISPATCH_SENTINEL_OFFSET,
    "runtime.S's own DISPATCH_SENTINEL_OFFSET must match sizeof(ProcSlot)");

extern "C" void runtimeBail(Runtime *runtime, uint32_t trapCode)
{
    register uint32_t trapCodeReg asm("r0") = trapCode;
    register uint32_t tagReg asm("r2") = LANDING_RESOURCE_ERROR;
    register uint32_t landingReg asm("r3") = runtime->sentinelLandingAddress();
    register uint32_t savedSpReg asm("r1") = runtime->savedSp();
    asm volatile("mov sp, r1\n\tbx r3" : : "r"(trapCodeReg), "r"(savedSpReg), "r"(tagReg), "r"(landingReg));
    __builtin_unreachable();
}
