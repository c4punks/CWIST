/** @file log_async.h
 * @brief Internal hook from the log call sites into the async log writer.
 */
#ifndef CWIST_SYS_APP_LOG_ASYNC_H
#define CWIST_SYS_APP_LOG_ASYNC_H

#include <stdbool.h>
#include <stddef.h>

/** @brief Queue one complete line for @p fd (STDOUT_FILENO or STDERR_FILENO).
 * @return true when queued; false when async logging is off, @p fd is not
 *         stdout/stderr, or the line is larger than the ring. The caller then
 *         writes the line synchronously itself. */
bool cwist_log_async_submit(int fd, const char *line, size_t len);

#endif
