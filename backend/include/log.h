/* log.h — minimal thread-safe levelled logging to stderr. */
#ifndef IRA_LOG_H
#define IRA_LOG_H

enum { LOG_DEBUG = 0, LOG_INFO, LOG_WARN, LOG_ERROR };

void log_set_level(int level);
#if defined(__GNUC__)
__attribute__((format(gnu_printf, 2, 3)))
#endif
void log_msg(int level, const char *fmt, ...);

#define LOGD(...) log_msg(LOG_DEBUG, __VA_ARGS__)
#define LOGI(...) log_msg(LOG_INFO, __VA_ARGS__)
#define LOGW(...) log_msg(LOG_WARN, __VA_ARGS__)
#define LOGE(...) log_msg(LOG_ERROR, __VA_ARGS__)

#endif
