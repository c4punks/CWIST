# Tutorial 10: Asynchronous Task Scheduler

Schedule asynchronous one-shot or recurring tasks using the background worker thread pool.

## Key Concepts
- Create a worker pool with `cwist_scheduler_create(worker_count, queue_capacity)`.
- Submit an immediate job with `cwist_scheduler_submit(s, fn, arg)`.
- Schedule a delayed or recurring job with `cwist_scheduler_schedule(s, fn, arg, delay_ms, interval_ms)`.
- Shut down and free the pool with `cwist_scheduler_destroy(s)`.

## Build and Run

```bash
mkdir build && cd build
cmake ..
make
./tut10
```
