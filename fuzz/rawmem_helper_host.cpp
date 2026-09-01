/* MEMMOVE's Thumb helper is ext_rawmem_helper.S, which no host build can
 * assemble. The host half never executes what it emits — the address is
 * only ever materialized as a literal — so a placeholder with the right
 * linkage is all the link needs. */
#include <cstdint>

extern "C" const uint16_t rawMemMoveHelper[1] = {0};
