/* fuzz — what a firing assert() does in the debug image.
 *
 * The shipped image is built -DNDEBUG (support/qemu-image/qemu-image.mk):
 * newlib's own __assert_func wants the fprintf path and a heap this design
 * has no room for. So nothing that executes emitted Thumb has ever been able
 * to see an assert fire — the host sink asserts but never runs the code, and
 * this image runs it but was blind. That is the gap this file closes, in a
 * separate ELF so the shipped one keeps its flash budget.
 *
 * Reported as an `A:` line in the result stream, so it lands in the failing
 * program's own ordinal slot and the driver names it without bisecting. The
 * run stops there: an assert means the translator's own invariants are
 * already gone, and there is nothing sound to continue into.
 */

#include <stdint.h>

#include "semihost.h"

extern "C" void __assert_func(const char *file, int line, const char *func, const char *expr)
{
    semihostWrite0("ASSERT ");
    semihostWrite0(file != nullptr ? file : "?");
    semihostWrite0(":");
    semihostWrite0(func != nullptr ? func : "?");
    semihostWrite0(": ");
    semihostWrite0(expr != nullptr ? expr : "?");
    semihostWrite0("\n");
    semihostWriteTagged("A:", (uint32_t)line);
    semihostExit(3);
}
