#ifndef JIT_ARMV6M_TEST_BC_BUFFERED_H_
#define JIT_ARMV6M_TEST_BC_BUFFERED_H_

#include <cstdint>

#include "bc_stub.h"

/* A block-buffered accessor, standing in for SPI NOR or a host link: the
 * program is not addressable, a handle is an offset into it, and the only
 * bytes in RAM are one block's worth. The block is deliberately far smaller
 * than any body, so every read of any length crosses boundaries.
 *
 * Nothing here is a cache: a block is fetched, used until the cursor leaves
 * it, and refetched if a cursor comes back to it. That is the weakest thing
 * an accessor can be and still be correct, which is the point. */

constexpr uint32_t BC_BLOCK_BYTES = 8;

/** Where the program lives on the "device". Handles are offsets into it. */
void bcBufferedAttach(const uint8_t *image, uint32_t len);

/** Block fetches since the last attach — the measure of how little of the
 *  program was ever resident. */
uint32_t bcBufferedFetches();

/** Hints seen since the last attach. */
uint32_t bcBufferedHints();

extern const BcDriver BC_BUFFERED;

#endif // JIT_ARMV6M_TEST_BC_BUFFERED_H_
