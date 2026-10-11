/** @file logger.c
 * @brief logger.c interface.
 */
#define _POSIX_C_SOURCE 200809L
#include <cwist/sys/app/logger.h>
#include <cwist/core/mem/alloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *level_strings[] = {"DEBUG", "INFO", "WARN", "ERROR"};
static const char *level_colors[] = {"\033[36m", "\033[32m", "\033[33m", "\033[31m"};
static const char *color_reset = "\033[0m";

/** @brief Returns the output stream for the given log level.
 * @param level Log level.
 * @return stderr for WARN and ERROR, stdout otherwise.
 */
static FILE *level_file(cwist_log_level_t level) {
    return level >= CWIST_LOG_WARN ? stderr : stdout;
}

/** @brief Creates a logger instance.
 * @param name Logger name copied into the instance; NULL selects "cwist".
 * @return Newly allocated logger on success, NULL on allocation failure.
 * @note Ownership of the returned logger transfers to the caller; release it
 *       with cwist_logger_destroy().
 */
cwist_logger *cwist_logger_create(const char *name) {
    cwist_logger *log = (cwist_logger *)cwist_alloc(sizeof(cwist_logger));
    if (!log) return NULL;
    log->name = cwist_strdup(name ? name : "cwist");
    if (!log->name) {
        cwist_free(log);
        return NULL;
    }
    log->level = CWIST_LOG_INFO;
    log->use_colors = true;
    return log;
}

/** @brief Destroys a logger instance and frees its name.
 * @param log Logger to free; NULL is safely ignored.
 */
void cwist_logger_destroy(cwist_logger *log) {
    if (!log) return;
    cwist_free(log->name);
    cwist_free(log);
}

/** @brief Sets the minimum log level that will be emitted.
 * @param log Logger to configure; NULL is safely ignored.
 * @param level New minimum level; messages below it are suppressed.
 */
void cwist_logger_set_level(cwist_logger *log, cwist_log_level_t level) {
    if (log) log->level = level;
}

/** @brief Enables or disables ANSI color output in log lines.
 * @param log Logger to configure; NULL is safely ignored.
 * @param enabled true to colorize level prefixes, false for plain output.
 */
void cwist_logger_set_colors(cwist_logger *log, bool enabled) {
    if (log) log->use_colors = enabled;
}

/** @brief Writes one log line: timestamp, level tag, logger name, message.
 * @param log Logger to log through; NULL is safely ignored.
 * @param level Level of the message; messages below the logger's level are suppressed.
 * @param fmt printf-style format string.
 * @param args Arguments for @p fmt.
 * @note Shared by cwist_logger_log() and the per-level wrappers, so every
 *       entry point formats the message the same way.
 */
static void logger_vlog(cwist_logger *log, cwist_log_level_t level, const char *fmt, va_list args) {
    if (!log || level < log->level || (unsigned)level > CWIST_LOG_ERROR) return;
    FILE *out = level_file(level);
    time_t now = time(NULL);
    struct tm tm_info;
    char time_buf[32];
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", localtime_r(&now, &tm_info));

    if (log->use_colors) {
        fprintf(out, "%s[%s]%s [%s] %s: ", level_colors[level], time_buf, color_reset,
                level_strings[level], log->name);
    } else {
        fprintf(out, "[%s] [%s] %s: ", time_buf, level_strings[level], log->name);
    }
    vfprintf(out, fmt, args);
    fprintf(out, "\n");
    fflush(out);
}

/** @brief Logs a formatted message at the given level.
 * @param log Logger to log through; NULL is safely ignored.
 * @param level Level of the message; messages below the logger's level are suppressed.
 * @param fmt printf-style format string.
 * @note The message is written to stdout for levels below WARN and to stderr
 *       otherwise, with a timestamp, level tag, and logger name prefix, followed by a newline.
 *       The shared FILE streams are not locked across the prefix and the message,
 *       so lines from concurrent threads may interleave.
 */
void cwist_logger_log(cwist_logger *log, cwist_log_level_t level, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    logger_vlog(log, level, fmt, args);
    va_end(args);
}

/** @brief Logs a formatted message at DEBUG level on stdout.
 * @param log Logger to log through; NULL is safely ignored.
 * @param fmt printf-style format string.
 */
void cwist_logger_debug(cwist_logger *log, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    logger_vlog(log, CWIST_LOG_DEBUG, fmt, args);
    va_end(args);
}

/** @brief Logs a formatted message at INFO level on stdout.
 * @param log Logger to log through; NULL is safely ignored.
 * @param fmt printf-style format string.
 */
void cwist_logger_info(cwist_logger *log, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    logger_vlog(log, CWIST_LOG_INFO, fmt, args);
    va_end(args);
}

/** @brief Logs a formatted message at WARN level on stderr.
 * @param log Logger to log through; NULL is safely ignored.
 * @param fmt printf-style format string.
 */
void cwist_logger_warn(cwist_logger *log, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    logger_vlog(log, CWIST_LOG_WARN, fmt, args);
    va_end(args);
}

/** @brief Logs a formatted message at ERROR level on stderr.
 * @param log Logger to log through; NULL is safely ignored.
 * @param fmt printf-style format string.
 */
void cwist_logger_error(cwist_logger *log, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    logger_vlog(log, CWIST_LOG_ERROR, fmt, args);
    va_end(args);
}
