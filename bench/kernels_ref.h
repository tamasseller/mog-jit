#ifndef JIT_ARMV6M_BENCH_KERNELS_REF_H_
#define JIT_ARMV6M_BENCH_KERNELS_REF_H_

#include <stdint.h>

/* Each returns the sample count it processed, as its DSL workload does.
 * All three are linked into every image; the generated REF_KERNEL macro
 * picks which one that image measures. */

uint32_t refPulseTrigger(uint32_t n);
uint32_t refIqPreamble(uint32_t n);
uint32_t refMedian5(uint32_t n);

#endif // JIT_ARMV6M_BENCH_KERNELS_REF_H_
