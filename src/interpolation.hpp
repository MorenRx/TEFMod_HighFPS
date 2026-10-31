#pragma once

#include "patchlib/type.h"
#include "patchlib/method.h"

namespace HighFPS {

struct Vector2 {
    float x;
    float y;
};

class InterpolationManager {
public:
    static bool init();

    // Called around Main.DoUpdate
    static void begin_keyframe();
    static void end_keyframe();

    // Called around Main.DoDraw
    static void apply_interpolation(float partial_tick);
    static void restore();

private:
    static patch_handle_t s_main_type;
    static patch_handle_t s_player_field;
    static patch_handle_t s_npc_field;
    static patch_handle_t s_projectile_field;
    static patch_handle_t s_gore_field;
    static patch_handle_t s_item_field;

    static patch_handle_t s_cached_player_arr;
    static patch_handle_t s_cached_npc_arr;
    static patch_handle_t s_cached_proj_arr;
    static patch_handle_t s_cached_gore_arr;
    static patch_handle_t s_cached_item_arr;

    static patch_handle_t get_player_array();
    static patch_handle_t get_npc_array();
    static patch_handle_t get_proj_array();
    static patch_handle_t get_gore_array();
    static patch_handle_t get_item_array();
};

} // namespace HighFPS
