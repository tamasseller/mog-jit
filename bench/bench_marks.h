#ifndef JIT_ARMV6M_BENCH_BENCH_MARKS_H_
#define JIT_ARMV6M_BENCH_BENCH_MARKS_H_

/* Region delimiters for bench/plugin/bench_plugin.c.
 *
 * A marker is an ordinary function the guest calls; the plugin registers a
 * callback at its address, read out of the ELF with nm. Nothing about the
 * mechanism is specific to compiled code, which is the point — the same
 * pair brackets a C kernel and an Executor::run whose body the JIT emitted
 * moments earlier.
 *
 * `noinline` keeps the symbol, `used` keeps it through -Os, and the empty
 * `asm volatile` keeps the call itself: without a barrier in the body GCC
 * is free to notice the function does nothing and drop the call, which
 * would silently measure a region that never opened.
 *
 * The markers' own instructions land inside the measured span, so every
 * count carries a fixed per-region bias. Do not try to reason it away —
 * measure it, with a marker pair around nothing at all, and subtract.
 */
#define BENCH_REGION_MARKERS(name)                                           \
    extern "C" __attribute__((noinline, used)) void bench_enter_##name(void) \
    {                                                                        \
        asm volatile("");                                                    \
    }                                                                        \
    extern "C" __attribute__((noinline, used)) void bench_exit_##name(void)  \
    {                                                                        \
        asm volatile("");                                                    \
    }

#endif // JIT_ARMV6M_BENCH_BENCH_MARKS_H_
