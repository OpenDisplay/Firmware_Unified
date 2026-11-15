/* Force-included (-include) into every shared/ translation unit on tc32-elf.
 *
 * tc32-elf-gcc is GCC 4.5.1; the __atomic builtins arrived in GCC 4.7. shared/
 * uses only __atomic_load_n / __atomic_store_n on naturally aligned scalars of
 * at most 32 bits, between the BLE IRQ and the main loop. TC32 is a single
 * in-order core without a data cache, so an aligned load or store of that width
 * is indivisible, and the only reordering left to prevent is the compiler's: a
 * volatile access between two compiler barriers gives acquire/release here.
 * Anything wider would not be atomic -- the size check rejects it at build time. */
#ifndef OD_TLSR_COMPAT_H
#define OD_TLSR_COMPAT_H

#if defined(__GNUC__) && (__GNUC__ < 4 || (__GNUC__ == 4 && __GNUC_MINOR__ < 7))

#define __ATOMIC_RELAXED 0
#define __ATOMIC_CONSUME 1
#define __ATOMIC_ACQUIRE 2
#define __ATOMIC_RELEASE 3
#define __ATOMIC_ACQ_REL 4
#define __ATOMIC_SEQ_CST 5

#define OD_TLSR_BARRIER() __asm__ __volatile__("" ::: "memory")
#define OD_TLSR_ATOMIC_SIZE_OK(p) \
    ((void)sizeof(char[(sizeof(*(p)) <= 4u) ? 1 : -1]))

#define __atomic_load_n(p, order)                                   \
    __extension__({                                                 \
        OD_TLSR_ATOMIC_SIZE_OK(p);                                  \
        (void)(order);                                              \
        OD_TLSR_BARRIER();                                          \
        __typeof__(*(p)) od_tlsr_v_ = *(volatile __typeof__(*(p)) *)(p); \
        OD_TLSR_BARRIER();                                          \
        od_tlsr_v_;                                                 \
    })

#define __atomic_store_n(p, v, order)                               \
    do {                                                            \
        OD_TLSR_ATOMIC_SIZE_OK(p);                                  \
        (void)(order);                                              \
        OD_TLSR_BARRIER();                                          \
        *(volatile __typeof__(*(p)) *)(p) = (v);                    \
        OD_TLSR_BARRIER();                                          \
    } while (0)

#endif

#endif
