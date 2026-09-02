#ifndef JIT_ARMV6M_RUNTIME_PROGRAM_FRAME_H_
#define JIT_ARMV6M_RUNTIME_PROGRAM_FRAME_H_

#include <stdint.h>

/* The two bytes a program carries after its last procedure, binding it to the
 * validator that produced it. Not a signature and not error correction: it
 * catches an off-by-one length, a buffer nobody filled in, a stale pointer,
 * and a producer built against a different contract. design.md §1.1. */

#define PROGRAM_FRAME_BYTES 2

/* Bump when the wire contract changes: an older producer's programs then stop
 * verifying instead of being misread. */
#define PROGRAM_CONTRACT_VERSION 3u

#define PROGRAM_FRAME_SEED (0x811C9DC5u ^ PROGRAM_CONTRACT_VERSION)

/* FNV-1a folded rather than truncated: the prime is odd, so bit 0 survives
 * every multiply and the raw low half is little more than a parity. */
inline uint16_t programFrameHash(const uint8_t *bytes, uint32_t len)
{
    uint32_t h = PROGRAM_FRAME_SEED;

    for(uint32_t i = 0; i < len; i++)
    {
        h ^= bytes[i];
        h *= 0x01000193u;
    }

    return (uint16_t)((h >> 16) ^ h);
}

/* size is the caller's own figure, and hashing exactly what it claims is what
 * checks it: a wrong one reads the stored value from the wrong place. Nothing
 * here reads past it. */
inline bool programFrameOk(const uint8_t *bytes, uint32_t size)
{
    if(size <= PROGRAM_FRAME_BYTES)
    {
        return false;
    }

    const uint32_t payload = size - PROGRAM_FRAME_BYTES;
    const uint16_t stored = (uint16_t)((uint16_t)bytes[payload] | (uint16_t)((uint16_t)bytes[payload + 1] << 8));

    return programFrameHash(bytes, payload) == stored;
}

#endif /* JIT_ARMV6M_RUNTIME_PROGRAM_FRAME_H_ */
