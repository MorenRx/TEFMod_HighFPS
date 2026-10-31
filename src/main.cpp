#include "mod_core.h"
#include "mod_logger.h"
#include "log.hpp"
#include "config.hpp"
#include "timing.hpp"
#include "ui.hpp"
#include "interpolation.hpp"

// Global logger pointer supplied by KernelLoader
extern "C" __attribute__((visibility("default")))
void (*mod_logger_write)(mod_log_level_t level, const char* tag, const char* fmt, ...) = nullptr;

static kernel_mod_info_t g_mod_info = {
    .pkg_id = "top.morenrx.highfps",
    .version_code = 1,
    .api_version = 1,
    .version = "1.0.0"
};

static void init_mod(kernel_mod_handle_t* handle) {
    if (handle && handle->private_dir) {
        HighFPS::log_init(handle->private_dir);
    }

    LOGI("==================================================");
    LOGI("HighFPS Mod starting initialization");
    LOGI("Package ID: %s, Version: %s", g_mod_info.pkg_id, g_mod_info.version);
    LOGI("Private directory: %s", (handle && handle->private_dir) ? handle->private_dir : "NULL");

    // 1. Initialize configuration
    if (handle && handle->private_dir) {
        HighFPS::g_config.init(handle->private_dir);
    } else {
        LOGW("handle or private_dir is NULL, using default config");
    }

    // 2. Initialize timing & frame rate unlocks
    if (!HighFPS::TimingManager::init()) {
        LOGE("Failed to initialize TimingManager");
    } else {
        LOGI("TimingManager initialized successfully");
    }

    // 3. Initialize UI setting hooks (Frame Rate Cap options)
    if (!HighFPS::UIManager::init()) {
        LOGE("Failed to initialize UIManager");
    } else {
        LOGI("UIManager initialized successfully");
    }

    // 4. Initialize motion interpolation
    if (!HighFPS::InterpolationManager::init()) {
        LOGE("Failed to initialize InterpolationManager");
    } else {
        LOGI("InterpolationManager initialized successfully");
    }

    LOGI("HighFPS Mod initialized successfully (Target FPS: %d)", HighFPS::g_config.target_fps);
    LOGI("==================================================");
}

static void cleanup_mod(kernel_mod_handle_t* handle) {
    LOGI("HighFPS Mod cleaning up");
}

static kernel_mod_info_t* get_info() {
    return &g_mod_info;
}

static kernel_mod_ops_t g_ops = {
    .init_mod = init_mod,
    .cleanup_mod = cleanup_mod,
    .get_info = get_info
};

extern "C" __attribute__((visibility("default")))
kernel_mod_ops_t* create_kernel_mod() {
    return &g_ops;
}
