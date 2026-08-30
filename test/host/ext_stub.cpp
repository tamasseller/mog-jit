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

extern "C" uint32_t extDecode(const uint8_t *bytes, uint32_t bytesLen, uint32_t offset, uint32_t *decl)
{
    if(active == nullptr || active->decode == nullptr)
    {
        return 0;
    }

    return active->decode(bytes, bytesLen, offset, decl);
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
