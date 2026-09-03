#include "ext_stub.h"
#include "assembler.h"
#include "runtime.h"

static const ExtStub *active = nullptr;

ExtScope::ExtScope(const ExtStub *stub): prev(active)
{
    active = stub;
}

ExtScope::~ExtScope()
{
    active = prev;
}

extern "C" bool extDescribe(uint8_t opcode, BcReader &wire, uint32_t *desc)
{
    if(active == nullptr || active->describe == nullptr)
    {
        return false;
    }

    return active->describe(opcode, wire, desc);
}

extern "C" void extEmit(ExtSite &site)
{
    if(active == nullptr || active->emit == nullptr)
    {
        runtimeBail(&site.a.runtime, RESOURCE_PROGRAM_EXT_UNKNOWN);
    }

    active->emit(site);
}

extern "C" uint32_t extHelperStackBytes()
{
    return active != nullptr ? active->helperStackBytes : 0;
}
