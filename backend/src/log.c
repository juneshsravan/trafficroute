#define _POSIX_C_SOURCE 200809L
#include "log.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <time.h>

static int g_level = LOG_INFO;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

void log_set_level(int level) { g_level = level; }

void log_msg(int level, const char *fmt, ...)
{
    if (level < g_level) return;
    static const char *names[] = {"DEBUG", "INFO", "WARN", "ERROR"};
    char ts[32];
    time_t now = time(NULL);
    struct tm tmv;
    gmtime_r(&now, &tmv);
    strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%SZ", &tmv);

    pthread_mutex_lock(&g_mu);
    fprintf(stderr, "%s %-5s ", ts, names[level < 0 ? 0 : level > 3 ? 3 : level]);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    pthread_mutex_unlock(&g_mu);
}
