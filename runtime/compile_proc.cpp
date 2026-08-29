#include <stdint.h>
#include "runtime_internal.h"
#include "translate_proc.h"
#include "assembler.h"

extern "C" void compileProc(uint32_t idx, Runtime *runtime)
{
    register uint32_t lruTick asm("r11");

    jitc::translateProc(idx, *runtime, lruTick);
}
