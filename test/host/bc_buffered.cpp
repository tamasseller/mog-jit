#include "bc_buffered.h"

#include <cassert>

namespace
{
const uint8_t *g_image = nullptr;
uint32_t g_imageLen = 0;

uint8_t g_block[BC_BLOCK_BYTES];
uint32_t g_blockAt = 0xffffffffu; // no block resident
uint32_t g_fetches = 0;
uint32_t g_hints = 0;

/* The one "device read". A cursor holds nothing but its offset, so a cursor
 * that comes back to a block someone else evicted simply causes another. */
void fetch(uint32_t at)
{
    const uint32_t base = at - at % BC_BLOCK_BYTES;

    for(uint32_t i = 0; i < BC_BLOCK_BYTES; i++)
    {
        const uint32_t src = base + i;
        g_block[i] = src < g_imageLen ? g_image[src] : 0;
    }

    g_blockAt = base;
    g_fetches++;
}

void bufferedOpen(BcCursor *c, BcHandle h, uint32_t)
{
    c->w = h;
}

uint8_t bufferedNext(BcCursor *c)
{
    const uint32_t at = c->w++;

    if(g_blockAt == 0xffffffffu || at < g_blockAt || at - g_blockAt >= BC_BLOCK_BYTES)
    {
        fetch(at);
    }

    return g_block[at - g_blockAt];
}

BcHandle bufferedTell(const BcCursor *c)
{
    return c->w;
}

void bufferedHint(BcHandle, uint32_t)
{
    g_hints++;
}
} // namespace

const BcDriver BC_BUFFERED = {bufferedOpen, bufferedNext, bufferedTell, bufferedHint};

void bcBufferedAttach(const uint8_t *image, uint32_t len)
{
    g_image = image;
    g_imageLen = len;
    g_blockAt = 0xffffffffu;
    g_fetches = 0;
    g_hints = 0;
}

uint32_t bcBufferedFetches()
{
    return g_fetches;
}

uint32_t bcBufferedHints()
{
    return g_hints;
}
