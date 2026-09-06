/* fuzz — target-side edge coverage.
 *
 * Host coverage (fuzz/coverage.sh) can only see what an x86 build links:
 * src/compiler, and the part of src/runtime that compiles off-target. The
 * dispatch path, runtime.S and the landing sequences run nowhere but here,
 * so nothing measured whether a campaign reaches them, or whether it has
 * stopped finding anything new.
 *
 * AFL's mechanism without AFL's plumbing: `-fsanitize-coverage=trace-pc`
 * calls this once per basic block, and the return address is hashed into a
 * bitmap. Not attributable to a source line — the question it answers is
 * "still finding new edges?", not "which line is cold". `trace-pc-guard`
 * would answer the second, and wants four bytes of RAM per edge, which this
 * budget does not have.
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
    const uint32_t pc = (uint32_t)(uintptr_t)__builtin_return_address(0);
    /* Knuth's multiplicative hash, high bits taken: the low bits of a Thumb
     * return address are mostly instruction alignment. */
    const uint32_t bit = (uint32_t)(pc * 2654435761u) >> (32 - COV_BITMAP_BITS_LOG2);
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

    /* The bitmap itself, so the host can union it across boots and see
     * whether a campaign is still turning up edges. Eight hex digits per
     * four bytes, one line, which is 512 characters for 256 bytes. */
    char buf[2 * COV_BITMAP_BYTES + 8];
    uint32_t at = 0;
    buf[at++] = 'C'; buf[at++] = 'O'; buf[at++] = 'V'; buf[at++] = ':';
    for(uint32_t i = 0; i < COV_BITMAP_BYTES; i++)
    {
        const uint8_t v = g_covBitmap[i];
        const uint32_t hi = v >> 4, lo = v & 0xf;
        buf[at++] = (char)(hi < 10 ? '0' + hi : 'a' + hi - 10);
        buf[at++] = (char)(lo < 10 ? '0' + lo : 'a' + lo - 10);
    }
    buf[at++] = '\n';
    buf[at] = '\0';
    semihostWrite0(buf);
}
