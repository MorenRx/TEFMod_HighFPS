#include "ui.hpp"
#include "config.hpp"
#include "timing.hpp"
#include "log.hpp"
#include "patchlib/type.h"
#include "patchlib/method.h"
#include "patchlib/struct/string.h"
#include "patchlib/struct/list.h"
#include "patchlib/struct/array.h"

#include <dlfcn.h>

namespace HighFPS {

constexpr int SETTING_ID_BATTERY_SAVING_MODE = 17; // GUISettingID.BatterySavingMode

static patch_handle_t s_pulldown_type = PATCH_NULL;
static patch_handle_t s_pulldown_inst = PATCH_NULL;
static patch_handle_t s_pulldown_ctor = PATCH_NULL;

typedef uint32_t (*il2cpp_gchandle_new_fn)(void* obj, bool pinned);
typedef void (*il2cpp_gchandle_free_fn)(uint32_t gchandle);

static il2cpp_gchandle_new_fn s_il2cpp_gchandle_new = nullptr;
static il2cpp_gchandle_free_fn s_il2cpp_gchandle_free = nullptr;

static void init_gchandle() {
    if (s_il2cpp_gchandle_new && s_il2cpp_gchandle_free) return;
    if (!s_il2cpp_gchandle_new) {
        void* sym = find_il2cpp_symbol("il2cpp_gchandle_new");
        if (sym) {
            s_il2cpp_gchandle_new = reinterpret_cast<il2cpp_gchandle_new_fn>(sym);
            LOGI("Found il2cpp_gchandle_new: %p", reinterpret_cast<void*>(s_il2cpp_gchandle_new));
        } else {
            LOGW("Failed to resolve il2cpp_gchandle_new");
        }
    }
    if (!s_il2cpp_gchandle_free) {
        void* sym = find_il2cpp_symbol("il2cpp_gchandle_free");
        if (sym) {
            s_il2cpp_gchandle_free = reinterpret_cast<il2cpp_gchandle_free_fn>(sym);
            LOGI("Found il2cpp_gchandle_free: %p", reinterpret_cast<void*>(s_il2cpp_gchandle_free));
        } else {
            LOGW("Failed to resolve il2cpp_gchandle_free");
        }
    }
}

static patch_handle_t s_pinned_fps_labels[ModConfig::FPS_COUNT] = { PATCH_NULL };
static uint32_t s_pinned_fps_gchandles[ModConfig::FPS_COUNT] = { 0 };
static patch_handle_t s_vanilla_title = PATCH_NULL;
static uint32_t s_vanilla_title_gchandle = 0;
static patch_handle_t s_fallback_title = PATCH_NULL;
static uint32_t s_fallback_title_gchandle = 0;
static patch_handle_t s_empty_string = PATCH_NULL;
static uint32_t s_empty_string_gchandle = 0;
static patch_handle_t s_title_key_str = PATCH_NULL;
static uint32_t s_title_key_gchandle = 0;
static uint32_t s_pulldown_gchandle = 0;

static patch_handle_t s_lang_type = PATCH_NULL;
static patch_handle_t s_get_text_val = PATCH_NULL;
static bool s_title_resolved = false;

static patch_handle_t create_pinned_string(const char* str, uint32_t* out_gchandle = nullptr) {
    if (!str) return PATCH_NULL;
    patch_handle_t managed_str = patchlib_string_create(str);
    if (managed_str && s_il2cpp_gchandle_new) {
        uint32_t handle = s_il2cpp_gchandle_new(managed_str, false);
        if (out_gchandle) {
            *out_gchandle = handle;
        }
    }
    return managed_str;
}

static patch_handle_t get_empty_string() {
    if (s_empty_string == PATCH_NULL) {
        init_gchandle();
        s_empty_string = create_pinned_string("", &s_empty_string_gchandle);
    }
    return s_empty_string;
}

static patch_handle_t get_fallback_title() {
    if (s_fallback_title == PATCH_NULL) {
        init_gchandle();
        s_fallback_title = create_pinned_string("Frame Rate Cap", &s_fallback_title_gchandle);
    }
    return s_fallback_title;
}

static patch_handle_t get_title_key_string() {
    if (s_title_key_str == PATCH_NULL) {
        init_gchandle();
        s_title_key_str = create_pinned_string("Mobile.FrameRateCapTitle", &s_title_key_gchandle);
    }
    return s_title_key_str;
}

static void ensure_pinned_fps_labels() {
    init_gchandle();
    for (int i = 0; i < ModConfig::FPS_COUNT; ++i) {
        if (s_pinned_fps_labels[i] == PATCH_NULL) {
            s_pinned_fps_labels[i] = create_pinned_string(ModConfig::FPS_LABELS[i], &s_pinned_fps_gchandles[i]);
        }
    }
    if (s_fallback_title == PATCH_NULL) {
        s_fallback_title = create_pinned_string("Frame Rate Cap", &s_fallback_title_gchandle);
    }
    if (s_empty_string == PATCH_NULL) {
        s_empty_string = create_pinned_string("", &s_empty_string_gchandle);
    }
    if (s_title_key_str == PATCH_NULL) {
        s_title_key_str = create_pinned_string("Mobile.FrameRateCapTitle", &s_title_key_gchandle);
    }
}

static patch_handle_t get_vanilla_title() {
    if (s_title_resolved) {
        return s_vanilla_title;
    }

    s_title_resolved = true;
    s_vanilla_title = PATCH_NULL;

    // Dynamic retrieval from Terraria's official Language system:
    // returns official localized text matching active game culture (zh-Hans: "最大帧率：", en-US: "Frame Rate Cap:", etc.)
    if (!s_lang_type) {
        s_lang_type = patchlib_type_get_type("Terraria.Localization", "Language");
    }
    if (s_lang_type) {
        if (!s_get_text_val) {
            s_get_text_val = patchlib_type_get_method_by_param_count(s_lang_type, "GetTextValue", 1);
        }
        if (s_get_text_val) {
            patch_handle_t key_str = get_title_key_string();
            if (key_str) {
                void* args[1] = { &key_str };
                patch_handle_t res_str = PATCH_NULL;
                if (patchlib_method_invoke_args(s_get_text_val, PATCH_NULL, &res_str, args) && res_str) {
                    init_gchandle();
                    if (s_il2cpp_gchandle_new) {
                        s_vanilla_title_gchandle = s_il2cpp_gchandle_new(res_str, false);
                    }
                    s_vanilla_title = res_str;
                    return s_vanilla_title;
                }
            }
        }
    }

    return s_vanilla_title;
}

static void invalidate_vanilla_title() {
    if (s_il2cpp_gchandle_free && s_vanilla_title_gchandle != 0) {
        s_il2cpp_gchandle_free(s_vanilla_title_gchandle);
        s_vanilla_title_gchandle = 0;
    }
    s_vanilla_title = PATCH_NULL;
    s_title_resolved = false;
}

static inline int get_setting_id(patch_handle_t instance) {
    if (!instance) return -1;
    // _id is at offset 0x10 in GUISettingEntry
    return *reinterpret_cast<const int*>(reinterpret_cast<const char*>(instance) + 0x10);
}

// ==============================================================================
// 1. GUIPulldownSetting Hooks (for Native Pulldown List UI)
// ==============================================================================

// Slot 9: GetNumberOfOptions()
static bool hook_Pulldown_GetNumberOfOptions_prefix(patch_handle_t instance, void** args,
                                                    const patch_method_signature_t* sig_info, void* result) {
    if (get_setting_id(instance) == SETTING_ID_BATTERY_SAVING_MODE) {
        *reinterpret_cast<int*>(result) = ModConfig::FPS_COUNT;
        return true; // Skip original
    }
    return false;
}

// Slot 11: GetOption(int index)
static bool hook_Pulldown_GetOption_prefix(patch_handle_t instance, void** args,
                                           const patch_method_signature_t* sig_info, void* result) {
    if (get_setting_id(instance) == SETTING_ID_BATTERY_SAVING_MODE) {
        ensure_pinned_fps_labels();
        int index = (args && args[0]) ? *reinterpret_cast<int*>(args[0]) : 1;
        patch_handle_t str_handle = PATCH_NULL;
        if (index >= 0 && index < ModConfig::FPS_COUNT) {
            str_handle = s_pinned_fps_labels[index];
        }
        if (!str_handle) {
            str_handle = s_pinned_fps_labels[1]; // default 60 FPS
        }
        if (!str_handle) {
            str_handle = patchlib_string_create((index >= 0 && index < ModConfig::FPS_COUNT) ? ModConfig::FPS_LABELS[index] : "60 FPS");
        }
        *reinterpret_cast<patch_handle_t*>(result) = str_handle;
        return true; // Skip original
    }
    return false;
}

// Slot 12: SetOption(int index)
static bool hook_Pulldown_SetOption_prefix(patch_handle_t instance, void** args,
                                           const patch_method_signature_t* sig_info, void* result) {
    if (get_setting_id(instance) == SETTING_ID_BATTERY_SAVING_MODE) {
        int index = (args && args[0]) ? *reinterpret_cast<int*>(args[0]) : 1;
        if (index >= 0 && index < ModConfig::FPS_COUNT) {
            g_config.set_fps_by_index(index);
            g_config.save();
            LOGI("GUIPulldownSetting option selected: %s (%d FPS)",
                 g_config.get_fps_label(), g_config.target_fps);
            TimingManager::apply_target_fps(g_config.target_fps);

            // Update selectedPosition (offset 0x40 in GUIPulldownSetting)
            *reinterpret_cast<int*>(reinterpret_cast<char*>(instance) + 0x40) = index;
            if (s_pulldown_inst != PATCH_NULL && s_pulldown_inst != instance) {
                *reinterpret_cast<int*>(reinterpret_cast<char*>(s_pulldown_inst) + 0x40) = index;
            }
            // Note: GUIPulldownSetting.Draw() manages its own closing and picking interceptor cleanup.
            // Do NOT call CloseOptions here as it unregisters _pickingInterceptor early and sets it to NULL,
            // which causes Draw() to pass NULL to UnregisterPickingInterceptor and crash with SIGSEGV (fault addr 0x4).
        }
        return true; // Skip original
    }
    return false;
}

// Slot 13: GetValue()
static bool hook_Pulldown_GetValue_prefix(patch_handle_t instance, void** args,
                                          const patch_method_signature_t* sig_info, void* result) {
    if (get_setting_id(instance) == SETTING_ID_BATTERY_SAVING_MODE) {
        ensure_pinned_fps_labels();
        int idx = g_config.get_fps_index();
        patch_handle_t str_handle = PATCH_NULL;
        if (idx >= 0 && idx < ModConfig::FPS_COUNT) {
            str_handle = s_pinned_fps_labels[idx];
        }
        if (!str_handle) {
            str_handle = s_pinned_fps_labels[1];
        }
        if (!str_handle) {
            str_handle = patchlib_string_create(g_config.get_fps_label());
        }
        *reinterpret_cast<patch_handle_t*>(result) = str_handle;
        return true; // Skip original
    }
    return false;
}

// GetTitle()
static bool hook_Pulldown_GetTitle_prefix(patch_handle_t instance, void** args,
                                          const patch_method_signature_t* sig_info, void* result) {
    if (get_setting_id(instance) == SETTING_ID_BATTERY_SAVING_MODE) {
        patch_handle_t str_handle = get_vanilla_title();
        if (!str_handle) {
            str_handle = get_fallback_title();
        }
        if (!str_handle) {
            str_handle = get_empty_string();
        }
        *reinterpret_cast<patch_handle_t*>(result) = str_handle;
        return true; // Skip original (vanilla GUIPulldownSetting has no case 17 and returns NULL)
    }
    return false;
}

// Slot 10: OptionDisabled(int index)
static bool hook_Pulldown_OptionDisabled_prefix(patch_handle_t instance, void** args,
                                                const patch_method_signature_t* sig_info, void* result) {
    if (get_setting_id(instance) == SETTING_ID_BATTERY_SAVING_MODE) {
        *reinterpret_cast<bool*>(result) = false; // All FPS options available
        return true; // Skip original
    }
    return false;
}

// ==============================================================================
// 2. GUIToggleSetting Hooks (Fallback & Safety)
// ==============================================================================

static inline patch_handle_t get_current_toggle_label() {
    ensure_pinned_fps_labels();
    int idx = g_config.get_fps_index();
    patch_handle_t str_handle = PATCH_NULL;
    if (idx >= 0 && idx < ModConfig::FPS_COUNT) {
        str_handle = s_pinned_fps_labels[idx];
    }
    if (!str_handle) {
        str_handle = s_pinned_fps_labels[1];
    }
    if (!str_handle) {
        str_handle = patchlib_string_create(g_config.get_fps_label());
    }
    return str_handle;
}

static bool hook_Toggle_GetOptionOn_prefix(patch_handle_t instance, void** args,
                                           const patch_method_signature_t* sig_info, void* result) {
    if (get_setting_id(instance) == SETTING_ID_BATTERY_SAVING_MODE) {
        *reinterpret_cast<patch_handle_t*>(result) = get_current_toggle_label();
        return true;
    }
    return false;
}

static bool hook_Toggle_GetOptionOff_prefix(patch_handle_t instance, void** args,
                                            const patch_method_signature_t* sig_info, void* result) {
    if (get_setting_id(instance) == SETTING_ID_BATTERY_SAVING_MODE) {
        *reinterpret_cast<patch_handle_t*>(result) = get_current_toggle_label();
        return true;
    }
    return false;
}

static bool hook_Toggle_GetSelected_prefix(patch_handle_t instance, void** args,
                                           const patch_method_signature_t* sig_info, void* result) {
    if (get_setting_id(instance) == SETTING_ID_BATTERY_SAVING_MODE) {
        *reinterpret_cast<bool*>(result) = (g_config.target_fps != 30);
        return true;
    }
    return false;
}

static bool hook_Toggle_Toggle_prefix(patch_handle_t instance, void** args,
                                      const patch_method_signature_t* sig_info, void* result) {
    if (get_setting_id(instance) == SETTING_ID_BATTERY_SAVING_MODE) {
        g_config.cycle_fps();
        g_config.save();
        LOGI("Frame Rate Cap toggled -> new setting: %s (%d FPS)",
             g_config.get_fps_label(), g_config.target_fps);
        TimingManager::apply_target_fps(g_config.target_fps);
        return true;
    }
    return false;
}

static void replace_entry_in_list(void* list_obj) {
    if (!list_obj || s_pulldown_inst == PATCH_NULL) return;

    // 1. Try patchlib struct APIs
    patch_handle_t items_arr = patchlib_list_get_array(list_obj);
    if (items_arr != PATCH_NULL) {
        size_t len = patchlib_array_length(items_arr);
        for (size_t i = 0; i < len; ++i) {
            patch_handle_t entry = PATCH_NULL;
            if (patchlib_array_at(items_arr, i, &entry) && entry != PATCH_NULL) {
                if (get_setting_id(entry) == SETTING_ID_BATTERY_SAVING_MODE) {
                    if (entry != s_pulldown_inst) {
                        // Inherit category (0x18) and sortId (0x14) from original entry so pulldown appears in correct tab and position
                        *reinterpret_cast<int*>(reinterpret_cast<char*>(s_pulldown_inst) + 0x14) =
                            *reinterpret_cast<const int*>(reinterpret_cast<const char*>(entry) + 0x14);
                        *reinterpret_cast<int*>(reinterpret_cast<char*>(s_pulldown_inst) + 0x18) =
                            *reinterpret_cast<const int*>(reinterpret_cast<const char*>(entry) + 0x18);

                        patchlib_array_set(items_arr, i, &s_pulldown_inst);
                        LOGI("Replaced BatterySavingMode entry with GUIPulldownSetting via patchlib array API at index %zu", i);
                    }
                }
            }
        }
    } else {
        // 2. Direct memory layout fallback for Il2Cpp List<T>
        void* items_ptr = *reinterpret_cast<void**>(reinterpret_cast<char*>(list_obj) + 0x10);
        int size = *reinterpret_cast<int*>(reinterpret_cast<char*>(list_obj) + 0x18);
        if (items_ptr && size > 0 && size < 10000) {
            void** items = reinterpret_cast<void**>(reinterpret_cast<char*>(items_ptr) + 0x20);
            for (int i = 0; i < size; ++i) {
                void* entry = items[i];
                if (entry && get_setting_id(entry) == SETTING_ID_BATTERY_SAVING_MODE) {
                    if (entry != s_pulldown_inst) {
                        // Inherit category (0x18) and sortId (0x14) from original entry
                        *reinterpret_cast<int*>(reinterpret_cast<char*>(s_pulldown_inst) + 0x14) =
                            *reinterpret_cast<const int*>(reinterpret_cast<const char*>(entry) + 0x14);
                        *reinterpret_cast<int*>(reinterpret_cast<char*>(s_pulldown_inst) + 0x18) =
                            *reinterpret_cast<const int*>(reinterpret_cast<const char*>(entry) + 0x18);

                        items[i] = s_pulldown_inst;
                        LOGI("Replaced BatterySavingMode entry with GUIPulldownSetting via direct memory at index %d", i);
                    }
                }
            }
        }
    }
}

static void ensure_pulldown_setting(patch_handle_t overlay_inst) {
    if (!overlay_inst) return;

    // Create GUIPulldownSetting instance if not created yet
    if (s_pulldown_inst == PATCH_NULL && s_pulldown_type && s_pulldown_ctor) {
        s_pulldown_inst = patchlib_type_new_instance(s_pulldown_type);
        if (s_pulldown_inst) {
            init_gchandle();
            if (s_il2cpp_gchandle_new) {
                s_pulldown_gchandle = s_il2cpp_gchandle_new(s_pulldown_inst, false);
                LOGI("Retained GUIPulldownSetting instance with normal GC handle: %p (handle=%u)", s_pulldown_inst, s_pulldown_gchandle);
            }
            int id = SETTING_ID_BATTERY_SAVING_MODE;
            void* null_cb = nullptr;
            void* ctor_args[] = { &id, &null_cb, &null_cb, &null_cb };
            patchlib_method_invoke_args(s_pulldown_ctor, s_pulldown_inst, nullptr, ctor_args);
            LOGI("Created GUIPulldownSetting instance for BatterySavingMode: %p", s_pulldown_inst);
        }
    }

    if (s_pulldown_inst == PATCH_NULL) return;

    // Keep selectedPosition in sync
    *reinterpret_cast<int*>(reinterpret_cast<char*>(s_pulldown_inst) + 0x40) = g_config.get_fps_index();

    // Offset 0x10 in GUISettingsOverlay is `private List<GUISettingEntry> _allSettings`
    void* all_settings = *reinterpret_cast<void**>(reinterpret_cast<char*>(overlay_inst) + 0x10);
    replace_entry_in_list(all_settings);

    // Offset 0x18 in GUISettingsOverlay is `public List<GUISettingEntry> _categorySettings`
    void* cat_settings = *reinterpret_cast<void**>(reinterpret_cast<char*>(overlay_inst) + 0x18);
    replace_entry_in_list(cat_settings);
}

static void hook_GUISettingsOverlay_Init_postfix(patch_handle_t instance, void** args, void* result,
                                                 const patch_method_signature_t* sig_info) {
    LOGI("GUISettingsOverlay.Init finished -> ensuring Pulldown setting is active");
    invalidate_vanilla_title();
    ensure_pulldown_setting(instance);
}

static bool hook_GUISettingsOverlay_SetCategory_prefix(patch_handle_t instance, void** args,
                                                       const patch_method_signature_t* sig_info, void* result) {
    ensure_pulldown_setting(instance);
    return false; // Continue original SetCategory
}

static void hook_GUISettingsOverlay_SetCategory_postfix(patch_handle_t instance, void** args, void* result,
                                                        const patch_method_signature_t* sig_info) {
    invalidate_vanilla_title();
    ensure_pulldown_setting(instance);
}

bool UIManager::init() {
    // 1. Resolve GUIPulldownSetting and hook its virtual methods
    s_pulldown_type = patchlib_type_get_type("", "GUIPulldownSetting");
    if (s_pulldown_type) {
        s_pulldown_ctor = patchlib_type_get_method_by_param_count(s_pulldown_type, ".ctor", 4);

        patch_handle_t get_num_opts = patchlib_type_get_method_by_param_count(s_pulldown_type, "GetNumberOfOptions", 0);
        if (get_num_opts) {
            patchlib_install_prepost_hook(get_num_opts, hook_Pulldown_GetNumberOfOptions_prefix, nullptr);
            LOGI("Hooked GUIPulldownSetting.GetNumberOfOptions");
        }

        patch_handle_t get_opt = patchlib_type_get_method_by_param_count(s_pulldown_type, "GetOption", 1);
        if (get_opt) {
            patchlib_install_prepost_hook(get_opt, hook_Pulldown_GetOption_prefix, nullptr);
            LOGI("Hooked GUIPulldownSetting.GetOption");
        }

        patch_handle_t set_opt = patchlib_type_get_method_by_param_count(s_pulldown_type, "SetOption", 1);
        if (set_opt) {
            patchlib_install_prepost_hook(set_opt, hook_Pulldown_SetOption_prefix, nullptr);
            LOGI("Hooked GUIPulldownSetting.SetOption");
        }

        patch_handle_t get_val = patchlib_type_get_method_by_param_count(s_pulldown_type, "GetValue", 0);
        if (get_val) {
            patchlib_install_prepost_hook(get_val, hook_Pulldown_GetValue_prefix, nullptr);
            LOGI("Hooked GUIPulldownSetting.GetValue");
        }

        patch_handle_t get_title = patchlib_type_get_method_by_param_count(s_pulldown_type, "GetTitle", 0);
        if (get_title) {
            patchlib_install_prepost_hook(get_title, hook_Pulldown_GetTitle_prefix, nullptr);
            LOGI("Hooked GUIPulldownSetting.GetTitle");
        }

        patch_handle_t opt_dis = patchlib_type_get_method_by_param_count(s_pulldown_type, "OptionDisabled", 1);
        if (opt_dis) {
            patchlib_install_prepost_hook(opt_dis, hook_Pulldown_OptionDisabled_prefix, nullptr);
            LOGI("Hooked GUIPulldownSetting.OptionDisabled");
        }
    } else {
        LOGW("Failed to resolve GUIPulldownSetting type");
    }

    // 2. Resolve GUISettingsOverlay to inject pulldown into _allSettings and _categorySettings
    patch_handle_t overlay_type = patchlib_type_get_type("", "GUISettingsOverlay");
    if (overlay_type) {
        patch_handle_t init_m = patchlib_type_get_method_by_param_count(overlay_type, "Init", 0);
        if (init_m) {
            patchlib_install_prepost_hook(init_m, nullptr, hook_GUISettingsOverlay_Init_postfix);
            LOGI("Hooked GUISettingsOverlay.Init");
        }

        patch_handle_t set_cat = patchlib_type_get_method_by_param_count(overlay_type, "SetCategory", 1);
        if (set_cat) {
            patchlib_install_prepost_hook(set_cat, hook_GUISettingsOverlay_SetCategory_prefix, hook_GUISettingsOverlay_SetCategory_postfix);
            LOGI("Hooked GUISettingsOverlay.SetCategory (pre+post)");
        }
        // Do NOT hook GUISettingsOverlay.Draw: modifying settings lists during rendering causes race conditions,
        // and hooking a no-op method introduces unnecessary per-frame libffi overhead.
    } else {
        LOGW("Failed to resolve GUISettingsOverlay type");
    }

    // 3. Fallback: Hook GUIToggleSetting methods (GetOption is NOT hooked to preserve vanilla title naturally)
    patch_handle_t toggle_setting_type = patchlib_type_get_type("", "GUIToggleSetting");
    if (toggle_setting_type) {
        patch_handle_t get_option_on = patchlib_type_get_method_by_param_count(toggle_setting_type, "GetOptionOn", 0);
        if (get_option_on) {
            patchlib_install_prepost_hook(get_option_on, hook_Toggle_GetOptionOn_prefix, nullptr);
        }

        patch_handle_t get_option_off = patchlib_type_get_method_by_param_count(toggle_setting_type, "GetOptionOff", 0);
        if (get_option_off) {
            patchlib_install_prepost_hook(get_option_off, hook_Toggle_GetOptionOff_prefix, nullptr);
        }

        patch_handle_t get_selected = patchlib_type_get_method_by_param_count(toggle_setting_type, "GetSelected", 0);
        if (get_selected) {
            patchlib_install_prepost_hook(get_selected, hook_Toggle_GetSelected_prefix, nullptr);
        }

        patch_handle_t toggle = patchlib_type_get_method_by_param_count(toggle_setting_type, "Toggle", 0);
        if (toggle) {
            patchlib_install_prepost_hook(toggle, hook_Toggle_Toggle_prefix, nullptr);
        }
    }

    ensure_pinned_fps_labels();
    return true;
}

} // namespace HighFPS
