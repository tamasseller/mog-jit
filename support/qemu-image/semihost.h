// The semihosting calls this runner needs. Deliberately a separate,
// dependency-free header rather than an extension of
// test/qemu/semihosting_output.h — that file is a 1test TestOutput
// implementation, and this image runs no tests.
//
// Only WRITE0 and EXIT, both of which pass their argument in R1 directly.
// The file operations (SYS_OPEN/SYS_READ/SYS_CLOSE/SYS_FLEN) are
// deliberately absent: SYS_OPEN returns -1 in this QEMU/machine
// combination for every path tried, the ":tt" stdin special case included,
// while WRITE0 works — so the batch is loaded straight into guest flash by
// `-device loader` instead (see exec_runner.cpp), which needs no
// semihosting at all and no protocol that could go wrong.
#ifndef JIT_ARMV6M_FUZZ_QEMU_EXEC_SEMIHOST_H_
#define JIT_ARMV6M_FUZZ_QEMU_EXEC_SEMIHOST_H_

#include <stdint.h>

/** Write a NUL-terminated string to the host's stdout. */
void semihostWrite0(const char *s);

[[noreturn]] void semihostExit(int code);

/** "<prefix><8 lowercase hex digits>\n" — this runner's whole output
 *  format, one line per program, parsed by qemu_exec.py. */
void semihostWriteTagged(const char *prefix, uint32_t v);

#endif // JIT_ARMV6M_FUZZ_QEMU_EXEC_SEMIHOST_H_
