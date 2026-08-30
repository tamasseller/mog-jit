// Wire bytes validateProgram refuses outright, before anything is translated.

#include <cstdint>

#include "executor.h"
#include "dispatch_abi.h"
#include "Test.h"

TEST(AProgramWithNoProceduresIsRejected)
{
    const uint8_t bytes[] = {0x00, 0x00, 0x00};
    ProgramResult r = Executor::onStack(0, /*interruptReserve=*/0).run(bytes, sizeof(bytes), nullptr, 0);

    CHECK(r.trapped);
    CHECK(r.value == RESOURCE_PROGRAM_NO_PROCS);
}

TEST(AnExtensionRangeOpcodeIsRejectedOnHardware)
{
    const uint8_t bytes[] = {0x01, 0x01, 0x01, 0x00, 0x80};
    ProgramResult r = Executor::onStack(0, /*interruptReserve=*/0).run(bytes, sizeof(bytes), nullptr, 0);

    CHECK(r.trapped);
    CHECK(r.value == RESOURCE_PROGRAM_EXT_UNKNOWN);
}
