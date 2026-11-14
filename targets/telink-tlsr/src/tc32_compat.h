/* Force-included into every OD-side translation unit (shared/ included) by the tc32 build.
 *
 * tc32-elf-gcc is GCC 4.5.1, which predates the __atomic builtins (GCC 4.7) that od_rxq and
 * od_xfer use for their producer/consumer ordering. TC32 is single-core and the only concurrency
 * is the BLE ISR against the main loop, so a volatile access fenced by compiler barriers gives
 * the same ordering: the barrier stops the compiler moving memory accesses across it, and the
 * core does not reorder them. */
#ifndef TC32_COMPAT_H
#define TC32_COMPAT_H

#ifndef __ATOMIC_RELAXED
#define __ATOMIC_RELAXED 0
#define __ATOMIC_ACQUIRE 2
#define __ATOMIC_RELEASE 3

#define __atomic_load_n(p, mo)                                        \
    ({ __typeof__(*(p)) od_v_ = *(volatile __typeof__(*(p)) *)(p);    \
       __asm__ volatile("" ::: "memory");                             \
       od_v_; })

#define __atomic_store_n(p, v, mo)                                    \
    do {                                                              \
        __asm__ volatile("" ::: "memory");                            \
        *(volatile __typeof__(*(p)) *)(p) = (v);                      \
        __asm__ volatile("" ::: "memory");                            \
    } while (0)
#endif

#endif /* TC32_COMPAT_H */
