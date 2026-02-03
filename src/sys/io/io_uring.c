#define _POSIX_C_SOURCE 200809L
#include <cwist/sys/io/cwist_io.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <signal.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <linux/io_uring.h>

/**
 * Minimal io_uring implementation without liburing dependency.
 * We use raw syscalls: io_uring_setup, io_uring_enter, io_uring_register.
 */

#ifndef __NR_io_uring_setup
#define __NR_io_uring_setup 425
#endif
#ifndef __NR_io_uring_enter
#define __NR_io_uring_enter 426
#endif

static inline int sys_io_uring_setup(unsigned entries, struct io_uring_params *p) {
    return (int)syscall(__NR_io_uring_setup, entries, p);
}

static inline int sys_io_uring_enter(int fd, unsigned to_submit, unsigned min_complete, unsigned flags, sigset_t *sig) {
    return (int)syscall(__NR_io_uring_enter, fd, to_submit, min_complete, flags, sig, _NSIG / 8);
}

// Barrier macros
#define read_barrier()  __asm__ __volatile__("":::"memory")
#define write_barrier() __asm__ __volatile__("":::"memory")

struct cwist_io_queue {
    int ring_fd;
    struct io_uring_params params;
    
    // Mapped Rings
    void *sq_ptr;
    void *cq_ptr;
    
    unsigned *sring_tail;
    unsigned *sring_mask;
    unsigned *sring_array;
    struct io_uring_sqe *sqes;
    
    unsigned *cring_head;
    unsigned *cring_tail;
    unsigned *cring_mask;
    struct io_uring_cqe *cqes;
};

cwist_io_queue *cwist_io_queue_create(size_t capacity) {
    cwist_io_queue *q = calloc(1, sizeof(cwist_io_queue));
    if (!q) return NULL;

    q->params.flags = 0;
    q->ring_fd = sys_io_uring_setup(capacity, &q->params);
    if (q->ring_fd < 0) {
        perror("io_uring_setup");
        free(q);
        return NULL;
    }

    // Map SQ and CQ
    size_t sring_sz = q->params.sq_off.array + q->params.sq_entries * sizeof(unsigned);
    size_t cring_sz = q->params.cq_off.cqes + q->params.cq_entries * sizeof(struct io_uring_cqe);

    q->sq_ptr = mmap(0, sring_sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, q->ring_fd, IORING_OFF_SQ_RING);
    if (q->sq_ptr == MAP_FAILED) goto err;

    q->cq_ptr = mmap(0, cring_sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, q->ring_fd, IORING_OFF_CQ_RING);
    if (q->cq_ptr == MAP_FAILED) goto err;

    q->sqes = mmap(0, q->params.sq_entries * sizeof(struct io_uring_sqe), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, q->ring_fd, IORING_OFF_SQES);
    if (q->sqes == MAP_FAILED) goto err;

    // Save Offsets
    q->sring_tail = q->sq_ptr + q->params.sq_off.tail;
    q->sring_mask = q->sq_ptr + q->params.sq_off.ring_mask;
    q->sring_array = q->sq_ptr + q->params.sq_off.array;
    
    q->cring_head = q->cq_ptr + q->params.cq_off.head;
    q->cring_tail = q->cq_ptr + q->params.cq_off.tail;
    q->cring_mask = q->cq_ptr + q->params.cq_off.ring_mask;
    q->cqes = q->cq_ptr + q->params.cq_off.cqes;

    return q;

err:
    perror("io_uring mmap");
    free(q);
    return NULL;
}

// User Data Wrapper
struct job_wrapper {
    cwist_job_func func;
    void *arg;
};

bool cwist_io_queue_submit(cwist_io_queue *q, cwist_job_func func, void *arg) {
    unsigned tail, index, mask;
    
    // Get SQE
    tail = *q->sring_tail;
    mask = *q->sring_mask;
    index = tail & mask;
    
    struct io_uring_sqe *sqe = &q->sqes[index];
    memset(sqe, 0, sizeof(*sqe));
    
    // For a generic job queue, strictly speaking io_uring is for IO.
    // However, we can use IORING_OP_NOP to just wake up the loop 
    // and store our job pointer in user_data.
    // Or simpler: We treat this "Job Queue" as an event loop.
    // But io_uring is async.
    
    struct job_wrapper *wrapper = malloc(sizeof(struct job_wrapper));
    wrapper->func = func;
    wrapper->arg = arg;

    sqe->opcode = IORING_OP_NOP;
    sqe->user_data = (unsigned long long)wrapper;
    
    // Commit SQE
    q->sring_array[index] = index;
    tail++;
    write_barrier();
    *q->sring_tail = tail;
    write_barrier();
    
    // Notify Kernel
    sys_io_uring_enter(q->ring_fd, 1, 0, 0, NULL);
    return true;
}

void cwist_io_queue_run(cwist_io_queue *q) {
    while (1) {
        // Wait for completion
        struct io_uring_cqe *cqe;
        unsigned head;
        
        // Wait for at least 1 event
        sys_io_uring_enter(q->ring_fd, 0, 1, IORING_ENTER_GETEVENTS, NULL);
        
        head = *q->cring_head;
        read_barrier();
        
        if (head != *q->cring_tail) {
            cqe = &q->cqes[head & *q->cring_mask];
            struct job_wrapper *job = (struct job_wrapper *)(uintptr_t)cqe->user_data;
            if (job) {
                job->func(job->arg);
                free(job);
            }
            head++;
            *q->cring_head = head;
            write_barrier();
        }
    }
}

void cwist_io_queue_destroy(cwist_io_queue *q) {
    if (q) {
        close(q->ring_fd);
        free(q);
    }
}
