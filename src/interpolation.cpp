#include "interpolation.hpp"
#include "timing.hpp"
#include "log.hpp"
#include "patchlib/field.h"
#include "patchlib/method.h"
#include <cmath>
#include <cstring>
#include <algorithm>

namespace HighFPS {

patch_handle_t InterpolationManager::s_main_type = PATCH_NULL;
patch_handle_t InterpolationManager::s_player_field = PATCH_NULL;
patch_handle_t InterpolationManager::s_npc_field = PATCH_NULL;
patch_handle_t InterpolationManager::s_projectile_field = PATCH_NULL;
patch_handle_t InterpolationManager::s_gore_field = PATCH_NULL;
patch_handle_t InterpolationManager::s_item_field = PATCH_NULL;

patch_handle_t InterpolationManager::s_cached_player_arr = PATCH_NULL;
patch_handle_t InterpolationManager::s_cached_npc_arr = PATCH_NULL;
patch_handle_t InterpolationManager::s_cached_proj_arr = PATCH_NULL;
patch_handle_t InterpolationManager::s_cached_gore_arr = PATCH_NULL;
patch_handle_t InterpolationManager::s_cached_item_arr = PATCH_NULL;

patch_handle_t InterpolationManager::get_player_array() {
    if (s_cached_player_arr == PATCH_NULL && s_player_field) {
        patchlib_field_get_value(s_player_field, PATCH_NULL, &s_cached_player_arr);
    }
    return s_cached_player_arr;
}

patch_handle_t InterpolationManager::get_npc_array() {
    if (s_cached_npc_arr == PATCH_NULL && s_npc_field) {
        patchlib_field_get_value(s_npc_field, PATCH_NULL, &s_cached_npc_arr);
    }
    return s_cached_npc_arr;
}

patch_handle_t InterpolationManager::get_proj_array() {
    if (s_cached_proj_arr == PATCH_NULL && s_projectile_field) {
        patchlib_field_get_value(s_projectile_field, PATCH_NULL, &s_cached_proj_arr);
    }
    return s_cached_proj_arr;
}

patch_handle_t InterpolationManager::get_gore_array() {
    if (s_cached_gore_arr == PATCH_NULL && s_gore_field) {
        patchlib_field_get_value(s_gore_field, PATCH_NULL, &s_cached_gore_arr);
    }
    return s_cached_gore_arr;
}

patch_handle_t InterpolationManager::get_item_array() {
    if (s_cached_item_arr == PATCH_NULL && s_item_field) {
        patchlib_field_get_value(s_item_field, PATCH_NULL, &s_cached_item_arr);
    }
    return s_cached_item_arr;
}

// Camera motion interpolation:
// In Terraria mobile Main.DoDraw (0x1105ECC), DoDraw_UpdateCameraPosition (0x110DBC8)
// dynamically updates the camera position from the local player's position every frame.
// Because the local player's position is already smoothly interpolated in apply_interpolation(),
// camera motion automatically tracks the interpolated player smoothly without manual overrides.

// Math helpers
static inline float lerp_f(float a, float b, float t) {
    return a + (b - a) * t;
}

static inline Vector2 lerp_vec2(const Vector2& a, const Vector2& b, float t) {
    return { a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t };
}

static inline float lerp_angle(float from, float to, float blend) {
    constexpr float PI2 = 6.28318530718f;
    constexpr float PI = 3.14159265359f;
    float diff = fmodf(to - from, PI2);
    if (diff > PI) diff -= PI2;
    else if (diff < -PI) diff += PI2;
    return from + diff * blend;
}

static inline bool should_interpolate_pos(const Vector2& a, const Vector2& b) {
    constexpr float MAX_TELEPORT_DIST = 128.0f;
    return (fabsf(a.x - b.x) <= MAX_TELEPORT_DIST) && (fabsf(a.y - b.y) <= MAX_TELEPORT_DIST);
}

// Keyframe structures
struct PlayerKeyframe {
    Vector2 pos;
    Vector2 item_loc;
    float item_rot;
    bool valid;
};

struct NPCKeyframe {
    Vector2 pos;
    float rot;
    bool valid;
};

struct ProjKeyframe {
    Vector2 pos;
    float rot;
    bool valid;
};

struct GoreKeyframe {
    Vector2 pos;
    float rot;
    bool valid;
};

struct ItemKeyframe {
    Vector2 pos;
    bool valid;
};

// Storage limits
constexpr size_t MAX_PLAYERS = 256;
constexpr size_t MAX_NPCS = 201;
constexpr size_t MAX_PROJS = 1001;
constexpr size_t MAX_GORES = 600;
constexpr size_t MAX_ITEMS = 401;

// Before & After buffers
static PlayerKeyframe s_player_before[MAX_PLAYERS];
static PlayerKeyframe s_player_after[MAX_PLAYERS];

static NPCKeyframe s_npc_before[MAX_NPCS];
static NPCKeyframe s_npc_after[MAX_NPCS];

static ProjKeyframe s_proj_before[MAX_PROJS];
static ProjKeyframe s_proj_after[MAX_PROJS];

static GoreKeyframe s_gore_before[MAX_GORES];
static GoreKeyframe s_gore_after[MAX_GORES];

static ItemKeyframe s_item_before[MAX_ITEMS];
static ItemKeyframe s_item_after[MAX_ITEMS];

static bool s_is_interpolated = false;

// Helper to access IL2CPP array elements
static inline void** get_array_items(patch_handle_t arr_handle, size_t& count, size_t max_limit) {
    if (!arr_handle) {
        count = 0;
        return nullptr;
    }
    const char* p = reinterpret_cast<const char*>(arr_handle);
    uint32_t len = *reinterpret_cast<const uint32_t*>(p + 0x18);
    count = std::min(static_cast<size_t>(len), max_limit);
    return reinterpret_cast<void**>(const_cast<char*>(p + 0x20));
}

// ======================== Keyframe Capture ========================

void InterpolationManager::begin_keyframe() {
    if (s_is_interpolated) {
        restore();
    }

    // 1. Players
    patch_handle_t player_arr = get_player_array();
    if (player_arr) {
        size_t count = 0;
        void** items = get_array_items(player_arr, count, MAX_PLAYERS);
        if (items) {
            for (size_t i = 0; i < count; ++i) {
                void* obj = items[i];
                if (!obj) {
                    s_player_before[i].valid = false;
                    continue;
                }
                char* p = reinterpret_cast<char*>(obj);
                bool active = *reinterpret_cast<const bool*>(p + 0x49);
                if (!active) {
                    s_player_before[i].valid = false;
                    continue;
                }
                s_player_before[i].pos = *reinterpret_cast<const Vector2*>(p + 0x14);
                s_player_before[i].item_loc = *reinterpret_cast<const Vector2*>(p + 0x4CC);
                s_player_before[i].item_rot = *reinterpret_cast<const float*>(p + 0x4C8);
                s_player_before[i].valid = true;
            }
        }
    }

    // 2. NPCs
    patch_handle_t npc_arr = get_npc_array();
    if (npc_arr) {
        size_t count = 0;
        void** items = get_array_items(npc_arr, count, MAX_NPCS);
        if (items) {
            for (size_t i = 0; i < count; ++i) {
                void* obj = items[i];
                if (!obj) {
                    s_npc_before[i].valid = false;
                    continue;
                }
                char* p = reinterpret_cast<char*>(obj);
                bool active = *reinterpret_cast<const bool*>(p + 0x49);
                if (!active) {
                    s_npc_before[i].valid = false;
                    continue;
                }
                s_npc_before[i].pos = *reinterpret_cast<const Vector2*>(p + 0x14);
                s_npc_before[i].rot = *reinterpret_cast<const float*>(p + 0x20C);
                s_npc_before[i].valid = true;
            }
        }
    }

    // 3. Projectiles
    patch_handle_t proj_arr = get_proj_array();
    if (proj_arr) {
        size_t count = 0;
        void** items = get_array_items(proj_arr, count, MAX_PROJS);
        if (items) {
            for (size_t i = 0; i < count; ++i) {
                void* obj = items[i];
                if (!obj) {
                    s_proj_before[i].valid = false;
                    continue;
                }
                char* p = reinterpret_cast<char*>(obj);
                bool active = *reinterpret_cast<const bool*>(p + 0x49);
                if (!active) {
                    s_proj_before[i].valid = false;
                    continue;
                }
                s_proj_before[i].pos = *reinterpret_cast<const Vector2*>(p + 0x14);
                s_proj_before[i].rot = *reinterpret_cast<const float*>(p + 0x60);
                s_proj_before[i].valid = true;
            }
        }
    }

    // 4. Gores
    patch_handle_t gore_arr = get_gore_array();
    if (gore_arr) {
        size_t count = 0;
        void** items = get_array_items(gore_arr, count, MAX_GORES);
        if (items) {
            for (size_t i = 0; i < count; ++i) {
                void* obj = items[i];
                if (!obj) {
                    s_gore_before[i].valid = false;
                    continue;
                }
                char* p = reinterpret_cast<char*>(obj);
                bool active = *reinterpret_cast<const bool*>(p + 0x34);
                if (!active) {
                    s_gore_before[i].valid = false;
                    continue;
                }
                s_gore_before[i].pos = *reinterpret_cast<const Vector2*>(p + 0x10);
                s_gore_before[i].rot = *reinterpret_cast<const float*>(p + 0x20);
                s_gore_before[i].valid = true;
            }
        }
    }

    // 5. Items
    patch_handle_t item_arr = get_item_array();
    if (item_arr) {
        size_t count = 0;
        void** items = get_array_items(item_arr, count, MAX_ITEMS);
        if (items) {
            for (size_t i = 0; i < count; ++i) {
                void* obj = items[i];
                if (!obj) {
                    s_item_before[i].valid = false;
                    continue;
                }
                char* p = reinterpret_cast<char*>(obj);
                int type = *reinterpret_cast<const int*>(p + 0x4C);
                int stack = *reinterpret_cast<const int*>(p + 0x60);
                if (type <= 0 || stack <= 0) {
                    s_item_before[i].valid = false;
                    continue;
                }
                s_item_before[i].pos = *reinterpret_cast<const Vector2*>(p + 0x14);
                s_item_before[i].valid = true;
            }
        }
    }
}

void InterpolationManager::end_keyframe() {
    // 1. Players
    patch_handle_t player_arr = get_player_array();
    if (player_arr) {
        size_t count = 0;
        void** items = get_array_items(player_arr, count, MAX_PLAYERS);
        if (items) {
            for (size_t i = 0; i < count; ++i) {
                void* obj = items[i];
                if (!obj || !s_player_before[i].valid) {
                    s_player_after[i].valid = false;
                    continue;
                }
                char* p = reinterpret_cast<char*>(obj);
                bool active = *reinterpret_cast<const bool*>(p + 0x49);
                if (!active) {
                    s_player_after[i].valid = false;
                    continue;
                }
                s_player_after[i].pos = *reinterpret_cast<const Vector2*>(p + 0x14);
                s_player_after[i].item_loc = *reinterpret_cast<const Vector2*>(p + 0x4CC);
                s_player_after[i].item_rot = *reinterpret_cast<const float*>(p + 0x4C8);
                s_player_after[i].valid = true;
            }
        }
    }

    // 2. NPCs
    patch_handle_t npc_arr = get_npc_array();
    if (npc_arr) {
        size_t count = 0;
        void** items = get_array_items(npc_arr, count, MAX_NPCS);
        if (items) {
            for (size_t i = 0; i < count; ++i) {
                void* obj = items[i];
                if (!obj || !s_npc_before[i].valid) {
                    s_npc_after[i].valid = false;
                    continue;
                }
                char* p = reinterpret_cast<char*>(obj);
                bool active = *reinterpret_cast<const bool*>(p + 0x49);
                if (!active) {
                    s_npc_after[i].valid = false;
                    continue;
                }
                s_npc_after[i].pos = *reinterpret_cast<const Vector2*>(p + 0x14);
                s_npc_after[i].rot = *reinterpret_cast<const float*>(p + 0x20C);
                s_npc_after[i].valid = true;
            }
        }
    }

    // 3. Projectiles
    patch_handle_t proj_arr = get_proj_array();
    if (proj_arr) {
        size_t count = 0;
        void** items = get_array_items(proj_arr, count, MAX_PROJS);
        if (items) {
            for (size_t i = 0; i < count; ++i) {
                void* obj = items[i];
                if (!obj || !s_proj_before[i].valid) {
                    s_proj_after[i].valid = false;
                    continue;
                }
                char* p = reinterpret_cast<char*>(obj);
                bool active = *reinterpret_cast<const bool*>(p + 0x49);
                if (!active) {
                    s_proj_after[i].valid = false;
                    continue;
                }
                s_proj_after[i].pos = *reinterpret_cast<const Vector2*>(p + 0x14);
                s_proj_after[i].rot = *reinterpret_cast<const float*>(p + 0x60);
                s_proj_after[i].valid = true;
            }
        }
    }

    // 4. Gores
    patch_handle_t gore_arr = get_gore_array();
    if (gore_arr) {
        size_t count = 0;
        void** items = get_array_items(gore_arr, count, MAX_GORES);
        if (items) {
            for (size_t i = 0; i < count; ++i) {
                void* obj = items[i];
                if (!obj || !s_gore_before[i].valid) {
                    s_gore_after[i].valid = false;
                    continue;
                }
                char* p = reinterpret_cast<char*>(obj);
                bool active = *reinterpret_cast<const bool*>(p + 0x34);
                if (!active) {
                    s_gore_after[i].valid = false;
                    continue;
                }
                s_gore_after[i].pos = *reinterpret_cast<const Vector2*>(p + 0x10);
                s_gore_after[i].rot = *reinterpret_cast<const float*>(p + 0x20);
                s_gore_after[i].valid = true;
            }
        }
    }

    // 5. Items
    patch_handle_t item_arr = get_item_array();
    if (item_arr) {
        size_t count = 0;
        void** items = get_array_items(item_arr, count, MAX_ITEMS);
        if (items) {
            for (size_t i = 0; i < count; ++i) {
                void* obj = items[i];
                if (!obj || !s_item_before[i].valid) {
                    s_item_after[i].valid = false;
                    continue;
                }
                char* p = reinterpret_cast<char*>(obj);
                int type = *reinterpret_cast<const int*>(p + 0x4C);
                int stack = *reinterpret_cast<const int*>(p + 0x60);
                if (type <= 0 || stack <= 0) {
                    s_item_after[i].valid = false;
                    continue;
                }
                s_item_after[i].pos = *reinterpret_cast<const Vector2*>(p + 0x14);
                s_item_after[i].valid = true;
            }
        }
    }
}

// ======================== Apply & Restore ========================

void InterpolationManager::apply_interpolation(float partial_tick) {
    s_is_interpolated = true;

    // 1. Players
    patch_handle_t player_arr = get_player_array();
    if (player_arr) {
        size_t count = 0;
        void** items = get_array_items(player_arr, count, MAX_PLAYERS);
        if (items) {
            for (size_t i = 0; i < count; ++i) {
                if (!s_player_before[i].valid || !s_player_after[i].valid) continue;
                void* obj = items[i];
                if (!obj) continue;
                char* p = reinterpret_cast<char*>(obj);

                if (should_interpolate_pos(s_player_before[i].pos, s_player_after[i].pos)) {
                    *reinterpret_cast<Vector2*>(p + 0x14) = lerp_vec2(s_player_before[i].pos, s_player_after[i].pos, partial_tick);
                }
                *reinterpret_cast<Vector2*>(p + 0x4CC) = lerp_vec2(s_player_before[i].item_loc, s_player_after[i].item_loc, partial_tick);
                *reinterpret_cast<float*>(p + 0x4C8) = lerp_angle(s_player_before[i].item_rot, s_player_after[i].item_rot, partial_tick);
            }
        }
    }

    // 2. NPCs
    patch_handle_t npc_arr = get_npc_array();
    if (npc_arr) {
        size_t count = 0;
        void** items = get_array_items(npc_arr, count, MAX_NPCS);
        if (items) {
            for (size_t i = 0; i < count; ++i) {
                if (!s_npc_before[i].valid || !s_npc_after[i].valid) continue;
                void* obj = items[i];
                if (!obj) continue;
                char* p = reinterpret_cast<char*>(obj);

                if (should_interpolate_pos(s_npc_before[i].pos, s_npc_after[i].pos)) {
                    *reinterpret_cast<Vector2*>(p + 0x14) = lerp_vec2(s_npc_before[i].pos, s_npc_after[i].pos, partial_tick);
                }
                *reinterpret_cast<float*>(p + 0x20C) = lerp_angle(s_npc_before[i].rot, s_npc_after[i].rot, partial_tick);
            }
        }
    }

    // 3. Projectiles
    patch_handle_t proj_arr = get_proj_array();
    if (proj_arr) {
        size_t count = 0;
        void** items = get_array_items(proj_arr, count, MAX_PROJS);
        if (items) {
            for (size_t i = 0; i < count; ++i) {
                if (!s_proj_before[i].valid || !s_proj_after[i].valid) continue;
                void* obj = items[i];
                if (!obj) continue;
                char* p = reinterpret_cast<char*>(obj);

                if (should_interpolate_pos(s_proj_before[i].pos, s_proj_after[i].pos)) {
                    *reinterpret_cast<Vector2*>(p + 0x14) = lerp_vec2(s_proj_before[i].pos, s_proj_after[i].pos, partial_tick);
                }
                *reinterpret_cast<float*>(p + 0x60) = lerp_angle(s_proj_before[i].rot, s_proj_after[i].rot, partial_tick);
            }
        }
    }

    // 4. Gores
    patch_handle_t gore_arr = get_gore_array();
    if (gore_arr) {
        size_t count = 0;
        void** items = get_array_items(gore_arr, count, MAX_GORES);
        if (items) {
            for (size_t i = 0; i < count; ++i) {
                if (!s_gore_before[i].valid || !s_gore_after[i].valid) continue;
                void* obj = items[i];
                if (!obj) continue;
                char* p = reinterpret_cast<char*>(obj);

                if (should_interpolate_pos(s_gore_before[i].pos, s_gore_after[i].pos)) {
                    *reinterpret_cast<Vector2*>(p + 0x10) = lerp_vec2(s_gore_before[i].pos, s_gore_after[i].pos, partial_tick);
                }
                *reinterpret_cast<float*>(p + 0x20) = lerp_angle(s_gore_before[i].rot, s_gore_after[i].rot, partial_tick);
            }
        }
    }

    // 5. Items
    patch_handle_t item_arr = get_item_array();
    if (item_arr) {
        size_t count = 0;
        void** items = get_array_items(item_arr, count, MAX_ITEMS);
        if (items) {
            for (size_t i = 0; i < count; ++i) {
                if (!s_item_before[i].valid || !s_item_after[i].valid) continue;
                void* obj = items[i];
                if (!obj) continue;
                char* p = reinterpret_cast<char*>(obj);

                if (should_interpolate_pos(s_item_before[i].pos, s_item_after[i].pos)) {
                    *reinterpret_cast<Vector2*>(p + 0x14) = lerp_vec2(s_item_before[i].pos, s_item_after[i].pos, partial_tick);
                }
            }
        }
    }
}

void InterpolationManager::restore() {
    if (!s_is_interpolated) return;
    s_is_interpolated = false;

    // 1. Players
    patch_handle_t player_arr = get_player_array();
    if (player_arr) {
        size_t count = 0;
        void** items = get_array_items(player_arr, count, MAX_PLAYERS);
        if (items) {
            for (size_t i = 0; i < count; ++i) {
                if (!s_player_after[i].valid) continue;
                void* obj = items[i];
                if (!obj) continue;
                char* p = reinterpret_cast<char*>(obj);

                *reinterpret_cast<Vector2*>(p + 0x14) = s_player_after[i].pos;
                *reinterpret_cast<Vector2*>(p + 0x4CC) = s_player_after[i].item_loc;
                *reinterpret_cast<float*>(p + 0x4C8) = s_player_after[i].item_rot;
            }
        }
    }

    // 2. NPCs
    patch_handle_t npc_arr = get_npc_array();
    if (npc_arr) {
        size_t count = 0;
        void** items = get_array_items(npc_arr, count, MAX_NPCS);
        if (items) {
            for (size_t i = 0; i < count; ++i) {
                if (!s_npc_after[i].valid) continue;
                void* obj = items[i];
                if (!obj) continue;
                char* p = reinterpret_cast<char*>(obj);

                *reinterpret_cast<Vector2*>(p + 0x14) = s_npc_after[i].pos;
                *reinterpret_cast<float*>(p + 0x20C) = s_npc_after[i].rot;
            }
        }
    }

    // 3. Projectiles
    patch_handle_t proj_arr = get_proj_array();
    if (proj_arr) {
        size_t count = 0;
        void** items = get_array_items(proj_arr, count, MAX_PROJS);
        if (items) {
            for (size_t i = 0; i < count; ++i) {
                if (!s_proj_after[i].valid) continue;
                void* obj = items[i];
                if (!obj) continue;
                char* p = reinterpret_cast<char*>(obj);

                *reinterpret_cast<Vector2*>(p + 0x14) = s_proj_after[i].pos;
                *reinterpret_cast<float*>(p + 0x60) = s_proj_after[i].rot;
            }
        }
    }

    // 4. Gores
    patch_handle_t gore_arr = get_gore_array();
    if (gore_arr) {
        size_t count = 0;
        void** items = get_array_items(gore_arr, count, MAX_GORES);
        if (items) {
            for (size_t i = 0; i < count; ++i) {
                if (!s_gore_after[i].valid) continue;
                void* obj = items[i];
                if (!obj) continue;
                char* p = reinterpret_cast<char*>(obj);

                *reinterpret_cast<Vector2*>(p + 0x10) = s_gore_after[i].pos;
                *reinterpret_cast<float*>(p + 0x20) = s_gore_after[i].rot;
            }
        }
    }

    // 5. Items
    patch_handle_t item_arr = get_item_array();
    if (item_arr) {
        size_t count = 0;
        void** items = get_array_items(item_arr, count, MAX_ITEMS);
        if (items) {
            for (size_t i = 0; i < count; ++i) {
                if (!s_item_after[i].valid) continue;
                void* obj = items[i];
                if (!obj) continue;
                char* p = reinterpret_cast<char*>(obj);

                *reinterpret_cast<Vector2*>(p + 0x14) = s_item_after[i].pos;
            }
        }
    }
}

// ======================== Main.DoDraw Hook ========================

static bool hook_Main_DoDraw_prefix(patch_handle_t instance, void** args,
                                   const patch_method_signature_t* sig_info, void* result) {
    float partial = TimingManager::get_partial_tick();
    InterpolationManager::apply_interpolation(partial);
    return false; // Run original DoDraw
}

static void hook_Main_DoDraw_postfix(patch_handle_t instance, void** args, void* result,
                                    const patch_method_signature_t* sig_info) {
    InterpolationManager::restore();
}

bool InterpolationManager::init() {
    s_main_type = patchlib_type_get_type("Terraria", "Main");
    if (!s_main_type) {
        LOGE("Failed to find Main type for InterpolationManager");
        return false;
    }

    s_player_field = patchlib_type_get_field(s_main_type, "player");
    s_npc_field = patchlib_type_get_field(s_main_type, "npc");
    s_projectile_field = patchlib_type_get_field(s_main_type, "projectile");
    s_gore_field = patchlib_type_get_field(s_main_type, "gore");
    s_item_field = patchlib_type_get_field(s_main_type, "item");

    patch_handle_t do_draw = patchlib_type_get_method_by_param_count(s_main_type, "DoDraw", 1);
    if (do_draw) {
        patchlib_install_prepost_hook(do_draw, hook_Main_DoDraw_prefix, hook_Main_DoDraw_postfix);
        LOGI("Hooked Main.DoDraw for motion interpolation");
    } else {
        LOGW("Failed to find Main.DoDraw");
    }

    return true;
}

} // namespace HighFPS
