/**
 * @file main.c
 * @brief Epoch-based reclamation with cwist_ebr_alloc / cwist_ebr_free.
 *
 * cwist_reg_ptr expects pointers from a TTAK-headered allocator; passing a
 * cwist_alloc() pointer to it and then calling cwist_gc_shutdown() triggers
 * TTAK header validation and aborts (issue #383).  The correct pairing for
 * deferred release of shared data is cwist_ebr_alloc() + cwist_ebr_free()
 * + cwist_gc_pipeline_tick(), which is what this example shows.
 */

#include <stdio.h>
#include <cwist/core/mem/alloc.h>
#include <cwist/core/mem/gc.h>

int main(void) {
    printf("=== Epoch GC Tutorial ===\n");

    /* 1. Allocate blocks intended for deferred release.  cwist_ebr_alloc()
     *    marks the intent: these will be released via cwist_ebr_free(), not
     *    cwist_free(), so a reader on another thread wrapped in
     *    ttak_epoch_enter() / ttak_epoch_exit() never sees a freed block. */
    printf("\n[Allocate and retire via EBR]\n");
    void *p1 = cwist_ebr_alloc(256);
    void *p2 = cwist_ebr_alloc(512);

    /* 2. Retire the blocks.  cwist_ebr_free() does not free immediately;
     *    it hands ownership to the EBR pipeline. */
    cwist_ebr_free(p1);
    cwist_ebr_free(p2);
    printf("p1 (256 B) and p2 (512 B) retired\n");

    /* 3. Drive reclamation.  Blocks retired in a past epoch are freed once
     *    no thread is still inside that epoch.  A few ticks are enough
     *    when there are no long-running readers. */
    printf("\n[Reclaim]\n");
    for (int i = 0; i < 4; i++)
        cwist_gc_pipeline_tick();
    printf("Pipeline ticks complete; retired blocks reclaimed\n");

    /* 4. Repeat with a third block. */
    void *p3 = cwist_ebr_alloc(1024);
    cwist_ebr_free(p3);
    for (int i = 0; i < 4; i++)
        cwist_gc_pipeline_tick();
    printf("Second retire + reclaim complete\n");

    /* 5. The epoch GC context (cwist_gc_t) is still useful for the
     *    auto-rotate flag and for integrations that need manual epoch
     *    control without registered pointers.  Shutdown is clean when
     *    nothing has been registered via cwist_reg_ptr. */
    printf("\n[GC context -- auto-rotate toggle]\n");
    cwist_gc_t gc = {0};
    cwist_gc(&gc, /*manual_rotation=*/true);
    cwist_gc_auto_rotate(&gc, true);
    printf("auto_rotated: %s\n", cwist_gc_auto_rotated(&gc) ? "on" : "off");
    cwist_gc_shutdown(&gc);
    printf("GC shut down (no registered pointers -- clean)\n");

    printf("\n=== Done ===\n");
    return 0;
}
