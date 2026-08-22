#include "TestRunner.h"
#include "PrintfOutput.h"

int main()
{
    return test::TestRunner::runAllTests(&test::PrintfOutput::instance) ? 0 : 1;
}
