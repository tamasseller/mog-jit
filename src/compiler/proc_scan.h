#ifndef JIT_ARMV6M_COMPILER_PROC_SCAN_H_
#define JIT_ARMV6M_COMPILER_PROC_SCAN_H_

#include <cstdint>

#include "resource_codes.h"
#include "ext.h"

namespace jitc
{

struct BodyScanResult
{
    uint32_t bodyBytes;  
    bool needsLRSave;    
    bool ok;             
    uint32_t failCode;   
};

BodyScanResult scanProcBody(const uint8_t *bytes, uint32_t maxBytes, uint32_t startOffset,
    uint32_t stackFloor = 0);

} // namespace jitc

#endif // JIT_ARMV6M_COMPILER_PROC_SCAN_H_
