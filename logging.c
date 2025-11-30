#include "logging.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <stdbool.h>

// Global log level (default: INFO)
static log_level_t g_log_level = LOG_INFO;
static pthread_mutex_t g_log_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool g_initialized = false;

// Log level names for output
static const char *level_names[] = {
    "ERROR",
    "WARN ",
    "INFO ",
    "DEBUG",
    "TRACE"
};

void log_init(void) {
    if (g_initialized) {
        return;
    }

    // Read log level from environment variable
    const char *env_level = getenv("PICKLE_LOG_LEVEL");
    if (env_level) {
        if (strcmp(env_level, "ERROR") == 0 || strcmp(env_level, "0") == 0) {
            g_log_level = LOG_ERROR;
        } else if (strcmp(env_level, "WARN") == 0 || strcmp(env_level, "1") == 0) {
            g_log_level = LOG_WARN;
        } else if (strcmp(env_level, "INFO") == 0 || strcmp(env_level, "2") == 0) {
            g_log_level = LOG_INFO;
        } else if (strcmp(env_level, "DEBUG") == 0 || strcmp(env_level, "3") == 0) {
            g_log_level = LOG_DEBUG;
        } else if (strcmp(env_level, "TRACE") == 0 || strcmp(env_level, "4") == 0) {
            g_log_level = LOG_TRACE;
        }
    }

    g_initialized = true;
}

void log_set_level(log_level_t level) {
    pthread_mutex_lock(&g_log_mutex);
    g_log_level = level;
    pthread_mutex_unlock(&g_log_mutex);
}

log_level_t log_get_level(void) {
    log_level_t level;
    pthread_mutex_lock(&g_log_mutex);
    level = g_log_level;
    pthread_mutex_unlock(&g_log_mutex);
    return level;
}

void log_message(log_level_t level, const char *component, const char *fmt, ...) {
    // Quick check without lock for performance
    if (level > g_log_level) {
        return;
    }

    // Thread-safe logging
    pthread_mutex_lock(&g_log_mutex);

    // Double-check after acquiring lock
    if (level > g_log_level) {
        pthread_mutex_unlock(&g_log_mutex);
        return;
    }

    // Print level and component
    fprintf(stderr, "[%s] [%s] ", level_names[level], component);

    // Print formatted message
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);

    // Always end with newline
    fprintf(stderr, "\n");
    fflush(stderr);

    pthread_mutex_unlock(&g_log_mutex);
}
