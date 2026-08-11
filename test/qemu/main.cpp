#include "TestRunner.h"
#include "semihosting_output.h"

int main()
{
    bool ok = test::TestRunner::runAllTests(&SemihostingOutput::instance);
    semihosting_exit(ok ? 0 : 1);
}
