#include "timing.hpp"
#include "config.hpp"
#include "interpolation.hpp"
#include "log.hpp"
#include "patchlib/field.h"
#include "patchlib/method.h"
#include "memdl/memdl.h"

#include <dlfcn.h>
#include <elf.h>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace HighFPS {

std::chrono::steady_clock::time_point TimingManager::s_last_tick_time = std::chrono::steady_clock::now();
patch_handle_t TimingManager::s_main_type = PATCH_NULL;
patch_handle_t TimingManager::s_game_type = PATCH_NULL;
patch_handle_t TimingManager::s_app_type = PATCH_NULL;
patch_handle_t TimingManager::s_runner_type = PATCH_NULL;
patch_handle_t TimingManager::s_qs_type = PATCH_NULL;

patch_handle_t TimingManager::s_set_target_fps_method = PATCH_NULL;
patch_handle_t TimingManager::s_get_target_fps_method = PATCH_NULL;
patch_handle_t TimingManager::s_set_vsync_method = PATCH_NULL;
patch_handle_t TimingManager::s_get_vsync_method = PATCH_NULL;

patch_handle_t TimingManager::s_runner_inst_field = PATCH_NULL;
patch_handle_t TimingManager::s_set_surface_rate_method = PATCH_NULL;
patch_handle_t TimingManager::s_apply_surface_rate_method = PATCH_NULL;
patch_handle_t TimingManager::s_game_paused_field = PATCH_NULL;
patch_handle_t TimingManager::s_game_high_fps_field = PATCH_NULL;
patch_handle_t TimingManager::s_game_update_mode_var_field = PATCH_NULL;
patch_handle_t TimingManager::s_runner_target_rate_field = PATCH_NULL;
patch_handle_t TimingManager::s_runner_pending_rate_field = PATCH_NULL;
patch_handle_t TimingManager::s_runner_disable_var_field = PATCH_NULL;

typedef void* (*il2cpp_resolve_icall_fn)(const char* name);
static il2cpp_resolve_icall_fn s_il2cpp_resolve_icall = nullptr;

typedef void (*set_targetFrameRate_fn)(int);
typedef int (*get_targetFrameRate_fn)();
typedef void (*set_vSyncCount_fn)(int);
typedef int (*get_vSyncCount_fn)();

static set_targetFrameRate_fn s_set_fps_icall = nullptr;
static get_targetFrameRate_fn s_get_fps_icall = nullptr;
static set_vSyncCount_fn s_set_vsync_icall = nullptr;
static get_vSyncCount_fn s_get_vsync_icall = nullptr;

static uint32_t s_frame_check_counter = 0;
static int s_last_enforced_fps = 0;

// Safe in-memory ELF symbol scanner for libil2cpp.so
static void* elf_find_symbol_in_memory(uintptr_t base_addr, const char* sym_name) {
    if (!base_addr) return nullptr;
    auto* ehdr = reinterpret_cast<const Elf64_Ehdr*>(base_addr);
    if (memcmp(ehdr->e_ident, ELFMAG, SELFMAG) != 0) return nullptr;

    auto* phdr = reinterpret_cast<const Elf64_Phdr*>(base_addr + ehdr->e_phoff);
    const Elf64_Dyn* dynamic = nullptr;
    for (size_t i = 0; i < ehdr->e_phnum; ++i) {
        if (phdr[i].p_type == PT_DYNAMIC) {
            dynamic = reinterpret_cast<const Elf64_Dyn*>(base_addr + phdr[i].p_vaddr);
            break;
        }
    }
    if (!dynamic) return nullptr;

    const char* strtab = nullptr;
    const Elf64_Sym* symtab = nullptr;
    size_t strsz = 0;
    size_t sym_count = 0;

    for (const Elf64_Dyn* d = dynamic; d->d_tag != DT_NULL; ++d) {
        if (d->d_tag == DT_STRTAB) {
            uintptr_t ptr = d->d_un.d_ptr;
            strtab = (ptr >= base_addr) ? reinterpret_cast<const char*>(ptr)
                                        : reinterpret_cast<const char*>(base_addr + ptr);
        } else if (d->d_tag == DT_SYMTAB) {
            uintptr_t ptr = d->d_un.d_ptr;
            symtab = (ptr >= base_addr) ? reinterpret_cast<const Elf64_Sym*>(ptr)
                                        : reinterpret_cast<const Elf64_Sym*>(base_addr + ptr);
        } else if (d->d_tag == DT_STRSZ) {
            strsz = d->d_un.d_val;
        } else if (d->d_tag == DT_HASH) {
            uintptr_t ptr = d->d_un.d_ptr;
            const uint32_t* hash = (ptr >= base_addr) ? reinterpret_cast<const uint32_t*>(ptr)
                                                      : reinterpret_cast<const uint32_t*>(base_addr + ptr);
            if (hash) {
                sym_count = hash[1]; // nchain in sysv hash table
            }
        }
    }

    if (!strtab || !symtab || strsz == 0) return nullptr;
    if (sym_count == 0 || sym_count > 100000) {
        sym_count = 10000;
    }

    for (size_t i = 0; i < sym_count; ++i) {
        uint32_t name_idx = symtab[i].st_name;
        if (name_idx >= strsz) continue;
        const char* name = strtab + name_idx;
        if (name && strcmp(name, sym_name) == 0) {
            return reinterpret_cast<void*>(base_addr + symtab[i].st_value);
        }
    }
    return nullptr;
}

// Find symbol in libil2cpp.so across namespace boundaries
void* find_il2cpp_symbol(const char* sym_name) {
    if (!sym_name) return nullptr;

    // 1. Try RTLD_DEFAULT
    void* sym = dlsym(RTLD_DEFAULT, sym_name);
    if (sym) {
        LOGI("Found %s via RTLD_DEFAULT: %p", sym_name, sym);
        return sym;
    }

    // 2. Try dlopen by soname
    void* handle = dlopen("libil2cpp.so", RTLD_NOLOAD);
    if (!handle) {
        handle = dlopen("libil2cpp.so", RTLD_NOW);
    }
    if (handle) {
        sym = dlsym(handle, sym_name);
        if (sym) {
            LOGI("Found %s via dlopen(\"libil2cpp.so\"): %p", sym_name, sym);
            return sym;
        }
    }

    // 3. Scan /proc/self/maps for libil2cpp.so path and base address
    FILE* fp = fopen("/proc/self/maps", "r");
    if (fp) {
        char line[512];
        char il2cpp_path[256] = {0};
        uintptr_t base_addr = 0;

        while (fgets(line, sizeof(line), fp)) {
            if (strstr(line, "libil2cpp.so")) {
                if (base_addr == 0) {
                    uintptr_t start = 0, end = 0;
                    if (sscanf(line, "%lx-%lx", &start, &end) == 2) {
                        base_addr = start;
                    }
                }
                char* path = strchr(line, '/');
                if (path && il2cpp_path[0] == '\0') {
                    size_t len = strlen(path);
                    while (len > 0 && (path[len - 1] == '\n' || path[len - 1] == '\r')) {
                        path[--len] = '\0';
                    }
                    if (len < sizeof(il2cpp_path)) {
                        strncpy(il2cpp_path, path, sizeof(il2cpp_path) - 1);
                    }
                }
            }
        }
        fclose(fp);

        if (il2cpp_path[0]) {
            LOGI("Found libil2cpp.so in maps: %s (base=%p)", il2cpp_path, reinterpret_cast<void*>(base_addr));

            handle = dlopen(il2cpp_path, RTLD_NOLOAD);
            if (!handle) handle = dlopen(il2cpp_path, RTLD_NOW);
            if (handle) {
                sym = dlsym(handle, sym_name);
                if (sym) {
                    LOGI("Found %s via dlopen(full_path): %p", sym_name, sym);
                    return sym;
                }
            }

            if (memdl_open_file && memdl_sym) {
                memdl_handle_t mhandle = memdl_open_file(il2cpp_path, MEMDL_NOW);
                if (mhandle) {
                    sym = memdl_sym(mhandle, sym_name);
                    if (sym) {
                        LOGI("Found %s via memdl_open_file: %p", sym_name, sym);
                        return sym;
                    }
                }
            }
        }

        if (base_addr) {
            sym = elf_find_symbol_in_memory(base_addr, sym_name);
            if (sym) {
                LOGI("Found %s via memory ELF scan: %p", sym_name, sym);
                return sym;
            }
        }
    }

    LOGW("Could not find %s symbol (will rely on patchlib fallback)", sym_name);
    return nullptr;
}

static void set_vsync_zero() {
    if (s_set_vsync_icall) {
        s_set_vsync_icall(0);
    }
    if (TimingManager::s_set_vsync_method) {
        int zero = 0;
        void* args[] = { &zero };
        patchlib_method_invoke_args(TimingManager::s_set_vsync_method, PATCH_NULL, nullptr, args);
    }
}

static void set_target_framerate(int fps) {
    if (s_set_fps_icall) {
        s_set_fps_icall(fps);
    }
    if (TimingManager::s_set_target_fps_method) {
        int target_val = fps;
        void* args[] = { &target_val };
        patchlib_method_invoke_args(TimingManager::s_set_target_fps_method, PATCH_NULL, nullptr, args);
    }
}

static patch_handle_t s_cached_runner_inst = PATCH_NULL;
static inline patch_handle_t get_runner_inst() {
    if (s_cached_runner_inst == PATCH_NULL && TimingManager::s_runner_inst_field) {
        patchlib_field_get_value(TimingManager::s_runner_inst_field, PATCH_NULL, &s_cached_runner_inst);
    }
    return s_cached_runner_inst;
}

// 1. Hook Game.UnityUpdate to enforce decoupled 60 TPS accumulator logic every frame
static bool hook_Game_UnityUpdate_prefix(patch_handle_t instance, void** args,
                                         const patch_method_signature_t* sig_info, void* result) {
    // Crucial: enforce runner DisableVariableUpdate = false and PendingTargetSurfaceRate = false
    // Game.UnityUpdate checks runner.DisableVariableUpdate at 0xE9E9EC BEFORE checking updateModeVariable!
    // If DisableVariableUpdate is true, it forces 1 tick/frame (speedup at >60 FPS).
    patch_handle_t runner_inst = get_runner_inst();
    if (runner_inst != PATCH_NULL) {
        if (TimingManager::s_runner_pending_rate_field) {
            bool pending = false;
            patchlib_field_set_value(TimingManager::s_runner_pending_rate_field, runner_inst, &pending);
        }
        if (TimingManager::s_runner_disable_var_field) {
            bool disable = false;
            patchlib_field_set_value(TimingManager::s_runner_disable_var_field, runner_inst, &disable);
        }
        *reinterpret_cast<bool*>(reinterpret_cast<char*>(runner_inst) + 0xB0) = false; // PendingTargetSurfaceRate = false
        *reinterpret_cast<bool*>(reinterpret_cast<char*>(runner_inst) + 0x2B) = false; // DisableVariableUpdate = false
    }

    if (instance != PATCH_NULL) {
        // Clear highFrameRate to prevent Thread.Sleep throttling (vanilla sets highFrameRate=true if FPS >= 67)
        if (TimingManager::s_game_high_fps_field) {
            bool false_val = false;
            patchlib_field_set_value(TimingManager::s_game_high_fps_field, instance, &false_val);
        }
        bool* highFrameRate = reinterpret_cast<bool*>(reinterpret_cast<char*>(instance) + 0x74);
        *highFrameRate = false;

        // Force updateModeVariable = true so the vanilla 60 Hz accumulator loop is executed
        if (TimingManager::s_game_update_mode_var_field) {
            bool true_val = true;
            patchlib_field_set_value(TimingManager::s_game_update_mode_var_field, instance, &true_val);
        }
        bool* updateModeVariable = reinterpret_cast<bool*>(reinterpret_cast<char*>(instance) + 0x53);
        *updateModeVariable = true;
    }

    // Guard: periodically verify and enforce VSync=0 and TargetFrameRate without spamming query -1
    ++s_frame_check_counter;
    if (s_frame_check_counter % 60 == 0) {
        if (s_last_enforced_fps != g_config.target_fps) {
            s_last_enforced_fps = g_config.target_fps;
            LOGI("Game.UnityUpdate guard: applying target FPS %d", g_config.target_fps);
            set_vsync_zero();
            set_target_framerate(g_config.target_fps);
        }
    }

    return false; // Continue executing original UnityUpdate
}

static void hook_Game_UnityUpdate_postfix(patch_handle_t instance, void** args, void* result,
                                          const patch_method_signature_t* sig_info) {
    // Vanilla's internal 1-second FPS monitor runs inside UnityUpdate and resets updateModeVariable=false
    // if FPS is between 56 and 62, and sets highFrameRate=true if FPS >= 67. Re-enforce them here.
    if (instance != PATCH_NULL) {
        if (TimingManager::s_game_high_fps_field) {
            bool false_val = false;
            patchlib_field_set_value(TimingManager::s_game_high_fps_field, instance, &false_val);
        }
        bool* highFrameRate = reinterpret_cast<bool*>(reinterpret_cast<char*>(instance) + 0x74);
        *highFrameRate = false;

        if (TimingManager::s_game_update_mode_var_field) {
            bool true_val = true;
            patchlib_field_set_value(TimingManager::s_game_update_mode_var_field, instance, &true_val);
        }
        bool* updateModeVariable = reinterpret_cast<bool*>(reinterpret_cast<char*>(instance) + 0x53);
        *updateModeVariable = true;
    }

    patch_handle_t runner_inst = get_runner_inst();
    if (runner_inst != PATCH_NULL) {
        *reinterpret_cast<bool*>(reinterpret_cast<char*>(runner_inst) + 0x2B) = false; // DisableVariableUpdate = false
        *reinterpret_cast<bool*>(reinterpret_cast<char*>(runner_inst) + 0xB0) = false; // PendingTargetSurfaceRate = false
    }
}

// 2. Hook Main.DoUpdate for tick time and keyframe recording
static bool hook_Main_DoUpdate_prefix(patch_handle_t instance, void** args,
                                      const patch_method_signature_t* sig_info, void* result) {
    TimingManager::s_last_tick_time = std::chrono::steady_clock::now();
    InterpolationManager::begin_keyframe();

    // Ensure runner has variable update ENABLED (DisableVariableUpdate = false)
    // so ticks are strictly driven by the 60 Hz accumulator and decoupled from render rate
    patch_handle_t runner_inst = get_runner_inst();
    if (runner_inst != PATCH_NULL) {
        if (TimingManager::s_runner_disable_var_field) {
            bool disable = false;
            patchlib_field_set_value(TimingManager::s_runner_disable_var_field, runner_inst, &disable);
        }
        *reinterpret_cast<bool*>(reinterpret_cast<char*>(runner_inst) + 0x2B) = false;
    }

    return false; // Run original DoUpdate
}

// Intercept and block Android Surface rate modifications to prevent locking panel refresh rate
static bool hook_XNAUnityRunner_SetAndroidSurfaceRate_prefix(patch_handle_t instance, void** args,
                                                            const patch_method_signature_t* sig_info, void* result) {
    return true; // Skip original
}

static bool hook_XNAUnityRunner_ApplyPendingSurfaceRate_prefix(patch_handle_t instance, void** args,
                                                              const patch_method_signature_t* sig_info, void* result) {
    return true; // Skip original
}

static void hook_Main_DoUpdate_postfix(patch_handle_t instance, void** args, void* result,
                                       const patch_method_signature_t* sig_info) {
    InterpolationManager::end_keyframe();
}

// 3. Hook Main.SetPlatform60FPS to override vanilla 30/60 FPS locking
static bool hook_Main_SetPlatform60FPS_prefix(patch_handle_t instance, void** args,
                                              const patch_method_signature_t* sig_info, void* result) {
    LOGI("Main.SetPlatform60FPS intercepted -> enforcing target FPS: %d", g_config.target_fps);
    TimingManager::apply_target_fps(g_config.target_fps);
    return true; // Skip original to prevent vanilla 30/60 override
}

// 4. Hook Main.set_Setting_Target60FPS to override vanilla setting changes
static bool hook_Main_set_Setting_Target60FPS_prefix(patch_handle_t instance, void** args,
                                                     const patch_method_signature_t* sig_info, void* result) {
    LOGI("Main.set_Setting_Target60FPS intercepted -> enforcing target FPS: %d", g_config.target_fps);
    TimingManager::apply_target_fps(g_config.target_fps);
    return true; // Skip original
}

// 5. Hook XNAUnityRunner.Awake to apply target FPS once instance is ready
static void hook_XNAUnityRunner_Awake_postfix(patch_handle_t instance, void** args, void* result,
                                              const patch_method_signature_t* sig_info) {
    LOGI("XNAUnityRunner.Awake completed -> applying target FPS: %d", g_config.target_fps);
    TimingManager::apply_target_fps(g_config.target_fps);
}

// 6. Hook Main.LoadSettings to preserve user frame rate choice
static void hook_Main_LoadSettings_postfix(patch_handle_t instance, void** args, void* result,
                                           const patch_method_signature_t* sig_info) {
    LOGI("Main.LoadSettings completed -> reapplying target FPS: %d", g_config.target_fps);
    TimingManager::apply_target_fps(g_config.target_fps);
}

// 7. Intercept QualitySettings.set_vSyncCount to prevent restoring VSync != 0
static bool hook_QualitySettings_set_vSyncCount_prefix(patch_handle_t instance, void** args,
                                                       const patch_method_signature_t* sig_info, void* result) {
    if (args && args[0]) {
        int* requested = reinterpret_cast<int*>(args[0]);
        if (*requested != 0) {
            LOGI("QualitySettings.set_vSyncCount(%d) intercepted -> changing argument to 0", *requested);
            *requested = 0;
        }
    }
    return false; // Allow original method to execute with value forced to 0
}

// 8. Intercept Application.set_targetFrameRate to prevent lowering FPS
static bool hook_Application_set_targetFrameRate_prefix(patch_handle_t instance, void** args,
                                                        const patch_method_signature_t* sig_info, void* result) {
    if (args && args[0]) {
        int* requested = reinterpret_cast<int*>(args[0]);
        if (*requested != g_config.target_fps) {
            LOGI("Application.set_targetFrameRate(%d) intercepted -> changing argument to %d",
                 *requested, g_config.target_fps);
            *requested = g_config.target_fps;
        }
    }
    return false; // Allow original method to execute with target_fps
}

bool TimingManager::init() {
    s_main_type = patchlib_type_get_type("Terraria", "Main");
    s_game_type = patchlib_type_get_type("Microsoft.Xna.Framework", "Game");
    s_app_type = patchlib_type_get_type("UnityEngine", "Application");
    s_qs_type = patchlib_type_get_type("UnityEngine", "QualitySettings");
    s_runner_type = patchlib_type_get_type("", "XNAUnityRunner");

    if (!s_main_type || !s_game_type) {
        LOGE("Failed to resolve Main or Game types");
        return false;
    }

    // Resolve native Unity icalls
    void* resolve_sym = find_il2cpp_symbol("il2cpp_resolve_icall");
    if (resolve_sym) {
        s_il2cpp_resolve_icall = reinterpret_cast<il2cpp_resolve_icall_fn>(resolve_sym);

        // QualitySettings.set_vSyncCount (internal calls are registered without parameter types)
        s_set_vsync_icall = reinterpret_cast<set_vSyncCount_fn>(
            s_il2cpp_resolve_icall("UnityEngine.QualitySettings::set_vSyncCount"));
        if (!s_set_vsync_icall) {
            s_set_vsync_icall = reinterpret_cast<set_vSyncCount_fn>(
                s_il2cpp_resolve_icall("UnityEngine.QualitySettings::set_vSyncCount(System.Int32)"));
        }

        s_get_vsync_icall = reinterpret_cast<get_vSyncCount_fn>(
            s_il2cpp_resolve_icall("UnityEngine.QualitySettings::get_vSyncCount"));
        if (!s_get_vsync_icall) {
            s_get_vsync_icall = reinterpret_cast<get_vSyncCount_fn>(
                s_il2cpp_resolve_icall("UnityEngine.QualitySettings::get_vSyncCount()"));
        }

        // Application.set_targetFrameRate
        s_set_fps_icall = reinterpret_cast<set_targetFrameRate_fn>(
            s_il2cpp_resolve_icall("UnityEngine.Application::set_targetFrameRate"));
        if (!s_set_fps_icall) {
            s_set_fps_icall = reinterpret_cast<set_targetFrameRate_fn>(
                s_il2cpp_resolve_icall("UnityEngine.Application::set_targetFrameRate(System.Int32)"));
        }

        s_get_fps_icall = reinterpret_cast<get_targetFrameRate_fn>(
            s_il2cpp_resolve_icall("UnityEngine.Application::get_targetFrameRate"));
        if (!s_get_fps_icall) {
            s_get_fps_icall = reinterpret_cast<get_targetFrameRate_fn>(
                s_il2cpp_resolve_icall("UnityEngine.Application::get_targetFrameRate()"));
        }

        LOGI("Resolved Unity icalls: set_vSyncCount=%p, get_vSyncCount=%p, set_targetFrameRate=%p, get_targetFrameRate=%p",
             s_set_vsync_icall, s_get_vsync_icall, s_set_fps_icall, s_get_fps_icall);
    }

    // Resolve QualitySettings via patchlib
    if (s_qs_type) {
        s_set_vsync_method = patchlib_type_get_method_by_param_count(s_qs_type, "set_vSyncCount", 1);
        s_get_vsync_method = patchlib_type_get_method_by_param_count(s_qs_type, "get_vSyncCount", 0);
        LOGI("Found QualitySettings methods: set_vSyncCount=%p, get_vsyncCount=%p",
             s_set_vsync_method, s_get_vsync_method);

        if (s_set_vsync_method) {
            patchlib_install_prepost_hook(s_set_vsync_method, hook_QualitySettings_set_vSyncCount_prefix, nullptr);
            LOGI("Hooked QualitySettings.set_vSyncCount successfully");
        }
    } else {
        LOGW("Failed to find UnityEngine.QualitySettings via patchlib");
    }

    // Resolve Application via patchlib
    if (s_app_type) {
        s_set_target_fps_method = patchlib_type_get_method_by_param_count(s_app_type, "set_targetFrameRate", 1);
        s_get_target_fps_method = patchlib_type_get_method_by_param_count(s_app_type, "get_targetFrameRate", 0);
        LOGI("Found Application methods: set_targetFrameRate=%p, get_targetFrameRate=%p",
             s_set_target_fps_method, s_get_target_fps_method);

        if (s_set_target_fps_method) {
            patchlib_install_prepost_hook(s_set_target_fps_method, hook_Application_set_targetFrameRate_prefix, nullptr);
            LOGI("Hooked Application.set_targetFrameRate successfully");
        }
    }

    // Resolve Game fields
    s_game_high_fps_field = patchlib_type_get_field(s_game_type, "highFrameRate");
    s_game_update_mode_var_field = patchlib_type_get_field(s_game_type, "updateModeVariable");
    LOGI("Found Game fields: highFrameRate=%p, updateModeVariable=%p",
         s_game_high_fps_field, s_game_update_mode_var_field);

    // Resolve XNAUnityRunner
    if (s_runner_type) {
        s_runner_inst_field = patchlib_type_get_field(s_runner_type, "_instance");
        s_set_surface_rate_method = patchlib_type_get_method_by_param_count(s_runner_type, "SetAndroidSurfaceRate", 1);
        s_apply_surface_rate_method = patchlib_type_get_method_by_param_count(s_runner_type, "ApplyPendingSurfaceRate", 0);

        s_runner_target_rate_field = patchlib_type_get_field(s_runner_type, "TargetSurfaceRate");
        s_runner_pending_rate_field = patchlib_type_get_field(s_runner_type, "PendingTargetSurfaceRate");
        s_runner_disable_var_field = patchlib_type_get_field(s_runner_type, "DisableVariableUpdate");

        if (s_set_surface_rate_method) {
            patchlib_install_prepost_hook(s_set_surface_rate_method, hook_XNAUnityRunner_SetAndroidSurfaceRate_prefix, nullptr);
            LOGI("Hooked XNAUnityRunner.SetAndroidSurfaceRate (blocked) successfully");
        }
        if (s_apply_surface_rate_method) {
            patchlib_install_prepost_hook(s_apply_surface_rate_method, hook_XNAUnityRunner_ApplyPendingSurfaceRate_prefix, nullptr);
            LOGI("Hooked XNAUnityRunner.ApplyPendingSurfaceRate (blocked) successfully");
        }

        patch_handle_t awake_method = patchlib_type_get_method_by_param_count(s_runner_type, "Awake", 0);
        if (awake_method) {
            patchlib_install_prepost_hook(awake_method, nullptr, hook_XNAUnityRunner_Awake_postfix);
            LOGI("Hooked XNAUnityRunner.Awake successfully");
        }
    }

    s_game_paused_field = patchlib_type_get_field(s_main_type, "gamePaused");

    // Hook Game.UnityUpdate
    patch_handle_t unity_update = patchlib_type_get_method_by_param_count(s_game_type, "UnityUpdate", 1);
    if (unity_update) {
        patchlib_install_prepost_hook(unity_update, hook_Game_UnityUpdate_prefix, hook_Game_UnityUpdate_postfix);
        LOGI("Hooked Game.UnityUpdate successfully");
    } else {
        LOGW("Failed to find Game.UnityUpdate");
    }

    // Hook Main.DoUpdate
    patch_handle_t do_update = patchlib_type_get_method_by_param_count(s_main_type, "DoUpdate", 1);
    if (do_update) {
        patchlib_install_prepost_hook(do_update, hook_Main_DoUpdate_prefix, hook_Main_DoUpdate_postfix);
        LOGI("Hooked Main.DoUpdate successfully");
    } else {
        LOGW("Failed to find Main.DoUpdate");
    }

    // Hook Main.SetPlatform60FPS
    patch_handle_t set_platform_fps = patchlib_type_get_method_by_param_count(s_main_type, "SetPlatform60FPS", 1);
    if (set_platform_fps) {
        patchlib_install_prepost_hook(set_platform_fps, hook_Main_SetPlatform60FPS_prefix, nullptr);
        LOGI("Hooked Main.SetPlatform60FPS successfully");
    } else {
        LOGW("Failed to find Main.SetPlatform60FPS");
    }

    // Hook Main.set_Setting_Target60FPS
    patch_handle_t set_setting_fps = patchlib_type_get_method_by_param_count(s_main_type, "set_Setting_Target60FPS", 1);
    if (set_setting_fps) {
        patchlib_install_prepost_hook(set_setting_fps, hook_Main_set_Setting_Target60FPS_prefix, nullptr);
        LOGI("Hooked Main.set_Setting_Target60FPS successfully");
    } else {
        LOGW("Failed to find Main.set_Setting_Target60FPS");
    }

    // Hook Main.LoadSettings
    patch_handle_t load_settings = patchlib_type_get_method_by_param_count(s_main_type, "LoadSettings", 1);
    if (load_settings) {
        patchlib_install_prepost_hook(load_settings, nullptr, hook_Main_LoadSettings_postfix);
        LOGI("Hooked Main.LoadSettings successfully");
    }

    // Initial apply
    apply_target_fps(g_config.target_fps);

    return true;
}

void TimingManager::apply_target_fps(int fps) {
    if (fps < 30) fps = 30;
    LOGI("=== TimingManager::apply_target_fps(%d) ===", fps);
    s_last_enforced_fps = fps;

    // 1. Force QualitySettings.vSyncCount = 0 so targetFrameRate is respected
    set_vsync_zero();
    LOGI("Applied QualitySettings.set_vSyncCount(0)");

    // 2. Set UnityEngine.Application.targetFrameRate
    set_target_framerate(fps);
    LOGI("Applied Application.set_targetFrameRate(%d)", fps);

    // 3. Ensure runner and game state maintain decoupled 60 TPS accumulator logic
    patch_handle_t runner_inst = get_runner_inst();
    if (runner_inst != PATCH_NULL) {
        // NEVER set surface rate or pending surface rate to avoid locking display panel refresh rate
        if (s_runner_pending_rate_field) {
            bool pending = false;
            patchlib_field_set_value(s_runner_pending_rate_field, runner_inst, &pending);
        }
        if (s_runner_disable_var_field) {
            bool disable = false;
            patchlib_field_set_value(s_runner_disable_var_field, runner_inst, &disable);
        }

        // Fallback offsets on runner
        *reinterpret_cast<bool*>(reinterpret_cast<char*>(runner_inst) + 0xB0) = false; // PendingTargetSurfaceRate = false
        *reinterpret_cast<bool*>(reinterpret_cast<char*>(runner_inst) + 0x2B) = false; // DisableVariableUpdate = false

        void* game_ptr = *reinterpret_cast<void**>(reinterpret_cast<char*>(runner_inst) + 0x80);
        if (game_ptr) {
            if (s_game_high_fps_field) {
                bool false_val = false;
                patchlib_field_set_value(s_game_high_fps_field, game_ptr, &false_val);
            }
            *reinterpret_cast<bool*>(reinterpret_cast<char*>(game_ptr) + 0x74) = false; // highFrameRate = false

            if (s_game_update_mode_var_field) {
                bool true_val = true;
                patchlib_field_set_value(s_game_update_mode_var_field, game_ptr, &true_val);
            }
            *reinterpret_cast<bool*>(reinterpret_cast<char*>(game_ptr) + 0x53) = true;  // updateModeVariable = true
        }
    }
}

bool TimingManager::is_game_paused() {
    if (s_game_paused_field) {
        bool paused = false;
        patchlib_field_get_value(s_game_paused_field, PATCH_NULL, &paused);
        return paused;
    }
    return false;
}

float TimingManager::get_partial_tick() {
    if (is_game_paused()) {
        return 1.0f;
    }

    auto now = std::chrono::steady_clock::now();
    double elapsed_sec = std::chrono::duration<double>(now - s_last_tick_time).count();
    float partial = static_cast<float>(elapsed_sec * 60.0);

    if (partial < 0.0f) partial = 0.0f;
    if (partial > 1.0f) partial = 1.0f;

    return partial;
}

} // namespace HighFPS
