#ifndef JIT_ARMV6M_TEST_EXT_STUB_H_
#define JIT_ARMV6M_TEST_EXT_STUB_H_

#include "ext.h"

/* The link-time ext symbols resolve here so a test can still swap
 * implementations per case; production builds bind them directly. */
struct ExtStub
{
    bool (*describe)(uint8_t opcode, BcReader &wire, uint32_t *desc);
    void (*emit)(ExtSite &site);
    uint32_t helperStackBytes;
};

class ExtScope
{
    const ExtStub *prev;

public:
    explicit ExtScope(const ExtStub *stub);
    ~ExtScope();

    ExtScope(const ExtScope &) = delete;
    ExtScope &operator=(const ExtScope &) = delete;
};

#endif // JIT_ARMV6M_TEST_EXT_STUB_H_
