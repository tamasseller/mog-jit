#ifndef JIT_ARMV6M_BENCH_KERNELS_REF_H_
#define JIT_ARMV6M_BENCH_KERNELS_REF_H_

#include <stdint.h>

/** Returns the sample count it processed, as the DSL workload does. */
uint32_t refPulseTrigger(uint32_t n);

#endif // JIT_ARMV6M_BENCH_KERNELS_REF_H_
