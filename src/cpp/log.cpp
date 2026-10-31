#include "log.hpp"
#include <cstdio>
#include <cstdarg>
#include <android/log.h>

namespace HighFPS {

void log_init(const char* private_dir) {
    // Logging routes through mod_logger_write (or __android_log_print fallback)
}

void log_write(mod_log_level_t level, const char* fmt, ...) {
    char buf[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    if (mod_logger_write) {
        mod_logger_write(level, HIGHFPS_TAG, "%s", buf);
    } else {
        android_LogPriority prio = ANDROID_LOG_INFO;
        switch (level) {
            case MOD_LOG_LEVEL_DEBUG:   prio = ANDROID_LOG_DEBUG; break;
            case MOD_LOG_LEVEL_INFO:    prio = ANDROID_LOG_INFO; break;
            case MOD_LOG_LEVEL_WARNING: prio = ANDROID_LOG_WARN; break;
            case MOD_LOG_LEVEL_ERROR:   prio = ANDROID_LOG_ERROR; break;
            default: break;
        }
        __android_log_print(prio, HIGHFPS_TAG, "%s", buf);
    }
}

} // namespace HighFPS
