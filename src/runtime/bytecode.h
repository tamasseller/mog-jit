#ifndef JIT_ARMV6M_RUNTIME_BYTECODE_H_
#define JIT_ARMV6M_RUNTIME_BYTECODE_H_

#include <stdint.h>
#include <cassert>

/* The only way anything reaches a program's bytes: a forward cursor, bound at
 * link time like ext.h's own seam, defaulting to memory-mapped in
 * bytecode_default.cpp. design.md §1.2 for why it is shaped this way. */

typedef uint32_t BcHandle;

/* One word the accessor owns outright; the core never reads it. Both drivers
 * in the tree keep a position there and everything else in statics, which is
 * what a no-heap target does anyway — widening this is a seam change. */
struct BcCursor
{
    uint32_t w;
};

extern "C" void bcOpen(BcCursor *c, BcHandle h, uint32_t len);
extern "C" uint8_t bcNext(BcCursor *c);

/** Names the byte `c` will read next, so a walk can pin a body it passes. */
extern "C" BcHandle bcTell(const BcCursor *c);

/** Advisory: `len` bytes from `h` are about to be read. */
extern "C" void bcHint(BcHandle h, uint32_t len);

/* One body's worth of cursor. End-of-body is the core's half of the contract:
 * every walk stops on atEnd(), so an accessor is never asked for a byte past
 * the length it was opened with. */
class BcReader
{
    BcCursor cursor;
    uint32_t left = 0;

public:
    inline void open(BcHandle h, uint32_t len)
    {
        bcOpen(&cursor, h, len);
        left = len;
    }

    inline uint8_t next()
    {
        assert(left); // GCOV_EXCL_LINE — the caller's own atEnd() is what bounds this
        left--;
        return bcNext(&cursor);
    }

    inline bool atEnd() const { return left == 0; }
    inline uint32_t remaining() const { return left; }

    inline BcHandle here() const { return bcTell(&cursor); }
};

#endif /* JIT_ARMV6M_RUNTIME_BYTECODE_H_ */
