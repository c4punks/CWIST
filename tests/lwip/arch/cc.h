/* Minimal lwIP compiler/platform glue for the adapter tests (host and
 * Cortex-M). lwip/arch.h supplies the integer types and byte order. */
#ifndef CWIST_TEST_LWIP_ARCH_CC_H
#define CWIST_TEST_LWIP_ARCH_CC_H

#include <stdio.h>
#include <stdlib.h>

#define LWIP_PLATFORM_DIAG(x) \
    do {                      \
        printf x;             \
    } while (0)
#define LWIP_PLATFORM_ASSERT(x)                                                  \
    do {                                                                         \
        printf("lwIP assertion \"%s\" failed at %s:%d\n", x, __FILE__, __LINE__); \
        abort();                                                                 \
    } while (0)

/* Initial sequence numbers and ports only; not a security boundary here. */
#define LWIP_RAND() ((u32_t)rand())

#endif /* CWIST_TEST_LWIP_ARCH_CC_H */
