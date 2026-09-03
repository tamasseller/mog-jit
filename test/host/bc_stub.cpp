#include "bc_stub.h"

namespace
{
/* Byte for byte what bytecode_default.cpp does — repeated rather than
 * reached, because these are the symbols that file defines and only one
 * definition can win a link. */
void mappedOpen(BcCursor *c, BcHandle h, uint32_t)
{
    c->w = h;
}

uint8_t mappedNext(BcCursor *c)
{
    return *(const uint8_t *)(uintptr_t)c->w++;
}

BcHandle mappedTell(const BcCursor *c)
{
    return c->w;
}

void mappedHint(BcHandle, uint32_t)
{
}

const BcDriver MAPPED = {mappedOpen, mappedNext, mappedTell, mappedHint};

const BcDriver *active = &MAPPED;
} // namespace

BcScope::BcScope(const BcDriver *driver): prev(active)
{
    active = driver != nullptr ? driver : &MAPPED;
}

BcScope::~BcScope()
{
    active = prev;
}

extern "C" void bcOpen(BcCursor *c, BcHandle h, uint32_t len)
{
    active->open(c, h, len);
}

extern "C" uint8_t bcNext(BcCursor *c)
{
    return active->next(c);
}

extern "C" BcHandle bcTell(const BcCursor *c)
{
    return active->tell(c);
}

extern "C" void bcHint(BcHandle h, uint32_t len)
{
    active->hint(h, len);
}
