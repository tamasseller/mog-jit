/* fuzz — target-side block coverage.
 *
 * Host coverage (fuzz/coverage.sh) can only see what an x86 build links:
 * src/compiler, and the part of src/runtime that compiles off-target. The
 * dispatch path, runtime.S and the landing sequences run nowhere but here,
 * so nothing measured whether a campaign reaches them, or whether it has
 * stopped finding anything new.
 *
 * AFL's mechanism without AFL's plumbing: `-fsanitize-coverage=trace-pc`
 * calls this once per basic block. AFL hashes the return address into a
 * small bitmap and lives with the collisions; there is no need to here.
 * Rom is 32KB and a block cannot start every two bytes — each opens with a
 * 4-byte `bl` — so a bit per four bytes of rom indexes every block
 * uniquely in 1KB of .bss. That makes the map exact in both directions: a
 * clear bit proves its block never ran, and `fuzz/ts/lib/cov_map.ts` turns
 * a bit number back into the address, function and source line.
 *
 * `trace-pc-guard` is the usual answer to the same problem and would cost
 * four bytes of ram per block; it is also Clang-only, and this is GCC.
 */

#include <stdint.h>

#include "cov_rt.h"
#include "semihost.h"

uint8_t g_covBitmap[COV_BITMAP_BYTES];

/* After `extern "C"`, not before it: an attribute in front of a linkage
   specification attaches to the specification and the function is
   instrumented anyway — which is a `bl` to itself as its first instruction. */
extern "C" __attribute__((no_sanitize_coverage)) void __sanitizer_cov_trace_pc(void)
{
    /* The Thumb bit does not need masking off: the return address of a
     * 2-aligned `bl` is 0 or 2 mod 4, so setting bit 0 never crosses the
     * boundary this shift divides on. */
    const uint32_t pc = (uint32_t)(uintptr_t)__builtin_return_address(0);
    const uint32_t bit = (pc >> 2) & (COV_BITMAP_BYTES * 8u - 1u);
    g_covBitmap[bit >> 3] |= (uint8_t)(1u << (bit & 7u));
}

__attribute__((no_sanitize_coverage)) void covReport(void)
{
    uint32_t set = 0;
    for(uint32_t i = 0; i < COV_BITMAP_BYTES; i++)
    {
        uint8_t b = g_covBitmap[i];
        while(b != 0) { set += b & 1u; b = (uint8_t)(b >> 1); }
    }
    semihostWriteTagged("COVBITS:", set);

    /* The bitmap itself, so the host can union it across boots and name what
     * a campaign never reached. Emitted in chunks: one buffer for the whole
     * map would be 2KB of stack in an image that has about 3KB of it. */
    static const char HEX[] = "0123456789abcdef";
    char buf[65];
    semihostWrite0("COV:");
    uint32_t at = 0;
    for(uint32_t i = 0; i < COV_BITMAP_BYTES; i++)
    {
        const uint8_t v = g_covBitmap[i];
        buf[at++] = HEX[v >> 4];
        buf[at++] = HEX[v & 0xf];
        if(at == sizeof(buf) - 1) { buf[at] = '\0'; semihostWrite0(buf); at = 0; }
    }
    buf[at] = '\0';
    semihostWrite0(buf);
    semihostWrite0("\n");
}
