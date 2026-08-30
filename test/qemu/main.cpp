#include "Test.h"
#include "semihosting_output.h"
#include "stack_paint.h"

int main(void)
{
    paintStack();

    bool ok = test::TestRunner::runAllTests(&SemihostingOutput::instance);
    ok = reportStackHighWaterMark() && ok;
    semihostingExit(ok ? 0 : 1);
    return 0;
}
