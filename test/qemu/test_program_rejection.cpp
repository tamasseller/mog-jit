// The wire rejections the image still makes for itself. Everything
// validateProgram already guarantees is an assert now (design.md §1.1), so
// what is left is the one thing the frame does not bind: which extension the
// image links.

#include <cstdint>

#include "executor.h"
#include "dispatch_abi.h"
#include "encode_instr.h"
#include "Test.h"

TEST(AnExtensionRangeOpcodeIsRejectedOnHardware)
{
    // 0xff, not 0x80: the image links the rawmem extension (support/ext-rawmem/ext_rawmem.cpp),
    // which claims 0x80-0x86 and declines everything else.
    const uint8_t literal[] = {0x01, 0x01, 0x01, 0x00, 0xff};
    const jitc::FramedProgram p = jitc::framedProgram(literal, sizeof(literal));

    ProgramResult r = Executor::onStack(0, /*interruptReserve=*/0).run(bcMapped(p.bytes), p.len, nullptr, 0);

    CHECK(r.trapped);
    CHECK(r.value == RESOURCE_PROGRAM_EXT_UNKNOWN);
}
