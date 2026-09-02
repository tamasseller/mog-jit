#ifndef JIT_ARMV6M_TEST_BC_STUB_H_
#define JIT_ARMV6M_TEST_BC_STUB_H_

#include "bytecode.h"

/* The link-time bytecode symbols resolve here so a test can swap accessors
 * per case; production builds bind bytecode_default.cpp directly. The mapped
 * accessor is what every other test in this suite gets, unchanged. */
struct BcDriver
{
    void (*open)(BcCursor *c, BcHandle h, uint32_t len);
    uint8_t (*next)(BcCursor *c);
    BcHandle (*tell)(const BcCursor *c);
    void (*hint)(BcHandle h, uint32_t len);
};

class BcScope
{
    const BcDriver *prev;

public:
    explicit BcScope(const BcDriver *driver);
    ~BcScope();

    BcScope(const BcScope &) = delete;
    BcScope &operator=(const BcScope &) = delete;
};

#endif // JIT_ARMV6M_TEST_BC_STUB_H_
