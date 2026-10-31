#pragma once

#include "mod_logger.h"

#define HIGHFPS_TAG "HighFPS"

namespace HighFPS {
    void log_init(const char* private_dir);
    void log_write(mod_log_level_t level, const char* fmt, ...);
}

#define LOGD(...) HighFPS::log_write(MOD_LOG_LEVEL_DEBUG, __VA_ARGS__)
#define LOGI(...) HighFPS::log_write(MOD_LOG_LEVEL_INFO, __VA_ARGS__)
#define LOGW(...) HighFPS::log_write(MOD_LOG_LEVEL_WARNING, __VA_ARGS__)
#define LOGE(...) HighFPS::log_write(MOD_LOG_LEVEL_ERROR, __VA_ARGS__)
