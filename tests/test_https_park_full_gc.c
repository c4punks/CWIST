/**
 * @file test_https_park_full_gc.c
 * @brief test_https_park.c with full GC on.
 *
 * Full GC latches once per process, so this is a separate binary. Under full
 * GC idle HTTP/1.1 connections and h2 sessions must still release the pool
 * thread: with parking off, one idle keep-alive client starved every new
 * connection on a single-thread pool for the idle budget (30 s for HTTP/1.1,
 * 300 s for h2), which surfaced as client timeouts (#344).
 */
#define TEST_HTTPS_PARK_FULL_GC 1
#include "test_https_park.c"
