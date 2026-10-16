#include <stdio.h>
#include <cwist/core/mem/alloc.h>
#include <cwist/core/mem/gc.h>

int main() {
    printf("=== Epoch GC Tutorial ===\n");

    /* 1. Initialise a GC context with manual epoch rotation */
    cwist_gc_t gc;
    cwist_gc(&gc, /*manual_rotation=*/true);
    printf("GC initialized (manual rotation mode)\n");

    /* 2. Allocate some buffers and register them with the GC. Registration
     *    hands ownership to the context: cwist_gc_shutdown() releases every
     *    registered block with cwist_free(). cwist_reg_ptr() is for
     *    cwist_alloc() blocks; ttak_mem_alloc()'d pointers would use the
     *    raw context (cwist_gc_raw) instead. */
    printf("\n[Register pointers]\n");
    void *p1 = cwist_alloc(256);
    void *p2 = cwist_alloc(512);
    cwist_reg_ptr(&gc, p1);
    cwist_reg_ptr_sized(&gc, p2, 512);
    printf("Registered p1 (256 B) and p2 (512 B)\n");

    /* 3. Manual epoch rotation — advances the epoch and runs a cleanup
     *    pass over the context's libttak-side bookkeeping. */
    printf("\n[Manual rotation]\n");
    cwist_gc_rotate(&gc);
    printf("Epoch rotated\n");

    /* 4. An explicit cwist_free() of a registered block detaches it from
     *    the context first (automatically) and then releases it. */
    cwist_free(p1);
    printf("Explicitly freed p1 (auto-unregistered)\n");

    /* 5. Allocate again and do another rotation */
    void *p3 = cwist_alloc(1024);
    cwist_reg_ptr_sized(&gc, p3, 1024);
    printf("Registered p3 (1024 B)\n");
    cwist_gc_rotate(&gc);
    printf("Second rotation complete\n");

    /* 6. Shut down — frees internal GC state plus every block still
     *    registered with the context (p2 and p3) via cwist_free(). */
    printf("\n[Shutdown]\n");
    cwist_gc_shutdown(&gc);
    printf("GC shut down (p2 and p3 reclaimed)\n");

    printf("\n=== Done ===\n");
    return 0;
}
