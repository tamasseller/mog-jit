// Wire bytes validateProgram refuses outright, before anything is translated.

#include <cstdint>

#include "executor.h"
#include "dispatch_abi.h"
#include "encode_instr.h"
#include "Test.h"

TEST(AProgramWithNoProceduresIsRejected)
{
    const uint8_t literal[] = {0x00, 0x00, 0x00};
    const jitc::FramedProgram p = jitc::framedProgram(literal, sizeof(literal));

    ProgramResult r = Executor::onStack(0, /*interruptReserve=*/0).run(p.bytes, p.len, nullptr, 0);

    CHECK(r.trapped);
    CHECK(r.value == RESOURCE_PROGRAM_NO_PROCS);
}

TEST(AnExtensionRangeOpcodeIsRejectedOnHardware)
{
    // 0xff, not 0x80: the image links the rawmem extension (test/ext_rawmem.cpp),
    // which claims 0x80-0x86 and declines everything else.
    const uint8_t literal[] = {0x01, 0x01, 0x01, 0x00, 0xff};
    const jitc::FramedProgram p = jitc::framedProgram(literal, sizeof(literal));

    ProgramResult r = Executor::onStack(0, /*interruptReserve=*/0).run(p.bytes, p.len, nullptr, 0);

    CHECK(r.trapped);
    CHECK(r.value == RESOURCE_PROGRAM_EXT_UNKNOWN);
}
