#ifndef LOGGING_H
#define LOGGING_H

#include <stdio.h>
#include <stdarg.h>

// Log levels (lower number = higher priority)
typedef enum {
    LOG_ERROR = 0,  // Critical errors that may cause crashes
    LOG_WARN  = 1,  // Warnings about potential issues
    LOG_INFO  = 2,  // Informational messages (default)
    LOG_DEBUG = 3,  // Detailed debugging information
    LOG_TRACE = 4   // Very verbose trace output
} log_level_t;

// Initialize logging system (call once at startup)
void log_init(void);

// Set the current log level (messages below this level are suppressed)
void log_set_level(log_level_t level);

// Get the current log level
log_level_t log_get_level(void);

// Core logging function (thread-safe)
void log_message(log_level_t level, const char *component, const char *fmt, ...);

// Convenience macros for each log level
#define LOG_ERROR(comp, ...) log_message(LOG_ERROR, comp, __VA_ARGS__)
#define LOG_WARN(comp, ...)  log_message(LOG_WARN, comp, __VA_ARGS__)
#define LOG_INFO(comp, ...)  log_message(LOG_INFO, comp, __VA_ARGS__)
#define LOG_DEBUG(comp, ...) log_message(LOG_DEBUG, comp, __VA_ARGS__)
#define LOG_TRACE(comp, ...) log_message(LOG_TRACE, comp, __VA_ARGS__)

#endif // LOGGING_H
