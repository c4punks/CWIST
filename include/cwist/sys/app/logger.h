/**
 * @file logger.h
 * @brief Application-level structured logging.
 */

#ifndef __CWIST_LOGGER_H__
#define __CWIST_LOGGER_H__

#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>

typedef enum {
    CWIST_LOG_DEBUG,
    CWIST_LOG_INFO,
    CWIST_LOG_WARN,
    CWIST_LOG_ERROR,
} cwist_log_level_t;

typedef struct cwist_logger {
    char *name;
    cwist_log_level_t level;
    bool use_colors;
} cwist_logger;

/** @name Lifecycle */
/** @{ */

/**
 * @brief Create a logger.
 */
cwist_logger *cwist_logger_create(const char *name);

/**
 * @brief Destroy a logger.
 */
void cwist_logger_destroy(cwist_logger *log);

/** @} */

/** @name Configuration */
/** @{ */

/**
 * @brief Set the minimum log level.
 */
void cwist_logger_set_level(cwist_logger *log, cwist_log_level_t level);

/**
 * @brief Enable or disable colored output.
 */
void cwist_logger_set_colors(cwist_logger *log, bool enabled);

/** @} */

/** @name Logging */
/** @{ */

/**
 * @brief Write a log message.
 */
void cwist_logger_log(cwist_logger *log, cwist_log_level_t level, const char *fmt, ...);

/**
 * @brief Write a debug message.
 */
void cwist_logger_debug(cwist_logger *log, const char *fmt, ...);

/**
 * @brief Write an info message.
 */
void cwist_logger_info(cwist_logger *log, const char *fmt, ...);

/**
 * @brief Write a warning message.
 */
void cwist_logger_warn(cwist_logger *log, const char *fmt, ...);

/**
 * @brief Write an error message.
 */
void cwist_logger_error(cwist_logger *log, const char *fmt, ...);

/** @} */

/** @name Async output
 * Opt-in background writer for cwist_logger_* lines and the built-in
 * access-log middleware (cwist_mw_access_log()). Callers format the line and
 * queue it; one writer thread does the write() calls, so the request path
 * makes no syscall. Lines stay whole and in order per stream, a full buffer
 * makes the caller wait (nothing is dropped), and a line larger than the
 * buffer is written synchronously.
 */
/** @{ */

/**
 * @brief Start the background log writer.
 * @param buffer_bytes Buffer size per stream (stdout and stderr); 0 selects 256 KiB.
 * @return true when async logging is active, false when it could not start
 *         (logging then stays synchronous).
 * @note Lines still queued at exit() are written by an atexit hook. Call
 *       cwist_log_async_flush() before _exit(). A forked child (CWIST_WORKERS)
 *       starts its own writer on its first line.
 */
bool cwist_log_async_start(size_t buffer_bytes);

/**
 * @brief Block until every line queued so far has been written.
 */
void cwist_log_async_flush(void);

/**
 * @brief Write out queued lines, stop the writer and return to synchronous output.
 */
void cwist_log_async_stop(void);

/**
 * @brief Whether the background log writer is active.
 */
bool cwist_log_async_active(void);

/** @} */

#endif
