#pragma once

#include "patchlib/type.h"
#include "patchlib/method.h"
#include "patchlib/field.h"
#include <chrono>

namespace HighFPS {

void* find_il2cpp_symbol(const char* sym_name);

class TimingManager {
public:
    static bool init();
    static void apply_target_fps(int fps);
    static float get_partial_tick();
    static bool is_game_paused();

    static std::chrono::steady_clock::time_point s_last_tick_time;

    static patch_handle_t s_main_type;
    static patch_handle_t s_game_type;
    static patch_handle_t s_app_type;
    static patch_handle_t s_runner_type;
    static patch_handle_t s_qs_type;

    static patch_handle_t s_set_target_fps_method;
    static patch_handle_t s_get_target_fps_method;
    static patch_handle_t s_set_vsync_method;
    static patch_handle_t s_get_vsync_method;

    static patch_handle_t s_runner_inst_field;
    static patch_handle_t s_set_surface_rate_method;
    static patch_handle_t s_apply_surface_rate_method;
    static patch_handle_t s_game_paused_field;
    static patch_handle_t s_game_high_fps_field;
    static patch_handle_t s_game_update_mode_var_field;
    static patch_handle_t s_runner_target_rate_field;
    static patch_handle_t s_runner_pending_rate_field;
    static patch_handle_t s_runner_disable_var_field;
};

} // namespace HighFPS
