#include "timing.hpp"
#include "config.hpp"
#include "interpolation.hpp"
#include "log.hpp"
#include "patchlib/field.h"
#include "patchlib/method.h"
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

patch_handle_t TimingManager::s_set_target_fps_method = PATCH_NULL;

patch_handle_t TimingManager::s_runner_inst_field = PATCH_NULL;
patch_handle_t TimingManager::s_runner_game_field = PATCH_NULL;
patch_handle_t TimingManager::s_main_instance_field = PATCH_NULL;
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
static set_targetFrameRate_fn s_set_fps_icall = nullptr;

static uint32_t s_frame_check_counter = 0;
static int s_last_enforced_fps = 0;
static bool s_initial_fps_applied = false;

// Safe in-memory ELF symbol scanner for libil2cpp.so
static void* elf_find_symbol_in_memory(uintptr_t base_addr, const char* sym_name) {
    if (!base_addr || !sym_name) return nullptr;
    auto* ehdr = reinterpret_cast<const Elf64_Ehdr*>(base_addr);
    if (memcmp(ehdr->e_ident, ELFMAG, SELFMAG) != 0) return nullptr;

    auto* phdr = reinterpret_cast<const Elf64_Phdr*>(base_addr + ehdr->e_phoff);
    uintptr_t min_vaddr = UINTPTR_MAX;
    uintptr_t dynamic_vaddr = 0;
    for (size_t i = 0; i < ehdr->e_phnum; ++i) {
        if (phdr[i].p_type == PT_LOAD) {
            if (phdr[i].p_vaddr < min_vaddr) {
                min_vaddr = phdr[i].p_vaddr;
            }
        } else if (phdr[i].p_type == PT_DYNAMIC) {
            dynamic_vaddr = phdr[i].p_vaddr;
        }
    }
    if (!dynamic_vaddr) return nullptr;

    uintptr_t load_bias = (min_vaddr != UINTPTR_MAX) ? (base_addr - min_vaddr) : base_addr;
    const auto* dynamic = reinterpret_cast<const Elf64_Dyn*>(load_bias + dynamic_vaddr);

    const char* strtab = nullptr;
    const Elf64_Sym* symtab = nullptr;
    size_t strsz = 0;
    size_t sym_count = 0;

    for (const Elf64_Dyn* d = dynamic; d->d_tag != DT_NULL; ++d) {
        if (d->d_tag == DT_STRTAB) {
            uintptr_t ptr = d->d_un.d_ptr;
            strtab = (ptr >= base_addr) ? reinterpret_cast<const char*>(ptr)
                                        : reinterpret_cast<const char*>(load_bias + ptr);
        } else if (d->d_tag == DT_SYMTAB) {
            uintptr_t ptr = d->d_un.d_ptr;
            symtab = (ptr >= base_addr) ? reinterpret_cast<const Elf64_Sym*>(ptr)
                                        : reinterpret_cast<const Elf64_Sym*>(load_bias + ptr);
        } else if (d->d_tag == DT_STRSZ) {
            strsz = d->d_un.d_val;
        } else if (d->d_tag == DT_HASH) {
            uintptr_t ptr = d->d_un.d_ptr;
            const uint32_t* hash = (ptr >= base_addr) ? reinterpret_cast<const uint32_t*>(ptr)
                                                      : reinterpret_cast<const uint32_t*>(load_bias + ptr);
            if (hash) {
                sym_count = hash[1]; // nchain in sysv hash table
            }
        } else if (d->d_tag == DT_GNU_HASH) {
            uintptr_t ptr = d->d_un.d_ptr;
            const uint32_t* gnu_hash = (ptr >= base_addr) ? reinterpret_cast<const uint32_t*>(ptr)
                                                          : reinterpret_cast<const uint32_t*>(load_bias + ptr);
            if (gnu_hash) {
                uint32_t nbuckets = gnu_hash[0];
                uint32_t symoffset = gnu_hash[1];
                uint32_t bloom_size = gnu_hash[2];
                // In 64-bit ELF, bloom filter entries are Elf64_Addr (8 bytes each = 2 * uint32_t)
                const uint32_t* buckets = gnu_hash + 4 + (bloom_size * 2);
                uint32_t max_chain_idx = 0;
                for (uint32_t b = 0; b < nbuckets; ++b) {
                    if (buckets[b] > max_chain_idx) {
                        max_chain_idx = buckets[b];
                    }
                }
                if (max_chain_idx >= symoffset) {
                    const uint32_t* chains = buckets + nbuckets;
                    uint32_t cur = max_chain_idx;
                    while (cur < 500000) {
                        uint32_t hashval = chains[cur - symoffset];
                        cur++;
                        if (hashval & 1) break; // End of chain marker
                    }
                    if (cur > sym_count) sym_count = cur;
                }
            }
        }
    }

    if (!strtab || !symtab || strsz == 0) return nullptr;
    if (sym_count == 0 || sym_count > 1000000) {
        sym_count = 50000;
    }

    for (size_t i = 0; i < sym_count; ++i) {
        if (symtab[i].st_value == 0) continue; // Undefined / imported symbol
        uint32_t name_idx = symtab[i].st_name;
        if (name_idx >= strsz) continue;
        const char* name = strtab + name_idx;
        if (name && strcmp(name, sym_name) == 0) {
            return reinterpret_cast<void*>(load_bias + symtab[i].st_value);
        }
    }
    return nullptr;
}

// Find symbol in libil2cpp.so across namespace boundaries safely
void* find_il2cpp_symbol(const char* sym_name) {
    if (!sym_name) return nullptr;

    // 1. Try RTLD_DEFAULT
    void* sym = dlsym(RTLD_DEFAULT, sym_name);
    if (sym) {
        LOGI("Found %s via RTLD_DEFAULT: %p", sym_name, sym);
        return sym;
    }

    // 2. Try dlopen by soname with RTLD_NOLOAD only
    // NEVER use RTLD_NOW here as it will force loading a duplicate copy into the mod namespace
    void* handle = dlopen("libil2cpp.so", RTLD_NOLOAD);
    if (handle) {
        sym = dlsym(handle, sym_name);
        if (sym) {
            LOGI("Found %s via dlopen(\"libil2cpp.so\", RTLD_NOLOAD): %p", sym_name, sym);
            return sym;
        }
    }

    // 3. Scan /proc/self/maps for the existing libil2cpp.so base address and resolve directly from memory.
    // In isolated linker namespaces (e.g., Root / LSPosed clns-9), dlopen(..., RTLD_NOW) creates an uninitialized
    // duplicate libil2cpp.so copy whose GC handle table is uninitialized, causing silent game crash on Locale zh-CN.
    // Resolving directly against the already-initialized memory image prevents duplicate instances.
    static uintptr_t s_cached_il2cpp_base = 0;
    static bool s_searched_maps = false;
    if (!s_cached_il2cpp_base && !s_searched_maps) {
        s_searched_maps = true;
        FILE* fp = fopen("/proc/self/maps", "r");
        if (fp) {
            char line[512];
            while (fgets(line, sizeof(line), fp)) {
                if (strstr(line, "libil2cpp.so")) {
                    char perms[8] = {0};
                    uintptr_t start = 0, end = 0, offset = 0;
                    if (sscanf(line, "%lx-%lx %7s %lx", &start, &end, perms, &offset) == 4) {
                        if (offset == 0 && perms[0] == 'r') {
                            auto* ehdr = reinterpret_cast<const Elf64_Ehdr*>(start);
                            if (memcmp(ehdr->e_ident, ELFMAG, SELFMAG) == 0) {
                                s_cached_il2cpp_base = start;
                                break;
                            }
                        }
                    }
                }
            }
            fclose(fp);
        }
    }

    if (s_cached_il2cpp_base) {
        LOGI("Found libil2cpp.so base in maps: %p", reinterpret_cast<void*>(s_cached_il2cpp_base));
        sym = elf_find_symbol_in_memory(s_cached_il2cpp_base, sym_name);
        if (sym) {
            LOGI("Found %s via memory ELF scan: %p", sym_name, sym);
            return sym;
        }
    }

    LOGW("Could not find %s symbol (will rely on patchlib fallback)", sym_name);
    return nullptr;
}

static patch_handle_t s_cached_runner_inst = PATCH_NULL;
static patch_handle_t s_cached_game_inst = PATCH_NULL;

static inline patch_handle_t get_runner_inst() {
    if (s_cached_runner_inst == PATCH_NULL && TimingManager::s_runner_inst_field) {
        patchlib_field_get_value(TimingManager::s_runner_inst_field, PATCH_NULL, &s_cached_runner_inst);
    }
    return s_cached_runner_inst;
}

static void set_target_framerate(int fps) {
    if (!s_set_fps_icall && s_il2cpp_resolve_icall) {
        s_set_fps_icall = reinterpret_cast<set_targetFrameRate_fn>(
            s_il2cpp_resolve_icall("UnityEngine.Application::set_targetFrameRate(System.Int32)"));
        if (!s_set_fps_icall) {
            s_set_fps_icall = reinterpret_cast<set_targetFrameRate_fn>(
                s_il2cpp_resolve_icall("UnityEngine.Application::set_targetFrameRate"));
        }
    }
    if (s_set_fps_icall) {
        s_set_fps_icall(fps);
    }
    if (TimingManager::s_set_target_fps_method) {
        int target_val = fps;
        void* args[] = { &target_val };
        patchlib_method_invoke_args(TimingManager::s_set_target_fps_method, PATCH_NULL, nullptr, args);
    }
}

// 1. Hook Game.UnityUpdate to enforce decoupled 60 TPS accumulator logic every frame
static bool hook_Game_UnityUpdate_prefix(patch_handle_t instance, void** args,
                                         const patch_method_signature_t* sig_info, void* result) {
    if (instance != PATCH_NULL) {
        s_cached_game_inst = instance;
    }

    // First frame fallback check if Awake didn't trigger apply_target_fps
    if (!s_initial_fps_applied) {
        LOGI("Game.UnityUpdate first-frame fallback -> applying target FPS: %d", g_config.target_fps);
        s_initial_fps_applied = true;
        TimingManager::apply_target_fps(g_config.target_fps);
    }

    // Enforce runner DisableVariableUpdate = false
    // Game.UnityUpdate checks runner.DisableVariableUpdate BEFORE checking updateModeVariable.
    // If DisableVariableUpdate is true, it forces 1 tick/frame (speedup at >60 FPS).
    patch_handle_t runner_inst = get_runner_inst();
    if (runner_inst != PATCH_NULL) {
        if (TimingManager::s_runner_disable_var_field) {
            bool disable = false;
            patchlib_field_set_value(TimingManager::s_runner_disable_var_field, runner_inst, &disable);
        }
    }

    if (instance != PATCH_NULL) {
        // Clear highFrameRate to prevent Thread.Sleep throttling (vanilla sets highFrameRate=true if FPS >= 67)
        if (TimingManager::s_game_high_fps_field) {
            bool false_val = false;
            patchlib_field_set_value(TimingManager::s_game_high_fps_field, instance, &false_val);
        }

        // Force updateModeVariable = true so the vanilla 60 Hz accumulator loop is executed
        if (TimingManager::s_game_update_mode_var_field) {
            bool true_val = true;
            patchlib_field_set_value(TimingManager::s_game_update_mode_var_field, instance, &true_val);
        }
    }

    // Guard: periodically verify and enforce TargetFrameRate and runner surface rate without reflection or log spam
    ++s_frame_check_counter;
    if (s_frame_check_counter % 60 == 0) {
        bool need_reapply = (s_last_enforced_fps != g_config.target_fps);
        if (!need_reapply && runner_inst != PATCH_NULL && TimingManager::s_runner_target_rate_field != PATCH_NULL) {
            float cur_surface_rate = 0.0f;
            patchlib_field_get_value(TimingManager::s_runner_target_rate_field, runner_inst, &cur_surface_rate);
            if (std::abs(cur_surface_rate - static_cast<float>(g_config.target_fps)) > 0.5f) {
                need_reapply = true;
            }
        }
        if (need_reapply) {
            LOGI("Game.UnityUpdate guard: reapplying target FPS %d", g_config.target_fps);
            TimingManager::apply_target_fps(g_config.target_fps);
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

        if (TimingManager::s_game_update_mode_var_field) {
            bool true_val = true;
            patchlib_field_set_value(TimingManager::s_game_update_mode_var_field, instance, &true_val);
        }
    }

    patch_handle_t runner_inst = get_runner_inst();
    if (runner_inst != PATCH_NULL) {
        if (TimingManager::s_runner_disable_var_field) {
            bool disable = false;
            patchlib_field_set_value(TimingManager::s_runner_disable_var_field, runner_inst, &disable);
        }
    }
}

// 2. Hook Main.DoUpdate for tick time and keyframe recording
static bool hook_Main_DoUpdate_prefix(patch_handle_t instance, void** args,
                                      const patch_method_signature_t* sig_info, void* result) {
    if (instance != PATCH_NULL && s_cached_game_inst == PATCH_NULL) {
        s_cached_game_inst = instance;
    }
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
    }

    return false; // Run original DoUpdate
}

// Intercept Android Surface rate modifications to ensure surface matches user target FPS
static bool hook_XNAUnityRunner_SetAndroidSurfaceRate_prefix(patch_handle_t instance, void** args,
                                                            const patch_method_signature_t* sig_info, void* result) {
    if (instance != PATCH_NULL && s_cached_runner_inst == PATCH_NULL) {
        s_cached_runner_inst = instance;
    }
    if (s_initial_fps_applied && args && args[0]) {
        float* rate_ptr = reinterpret_cast<float*>(args[0]);
        float target_fps_f = static_cast<float>(g_config.target_fps);
        if (std::abs(*rate_ptr - target_fps_f) > 0.01f) {
            LOGI("XNAUnityRunner.SetAndroidSurfaceRate(%.1f) intercepted -> changing argument to %.1f",
                 *rate_ptr, target_fps_f);
            *rate_ptr = target_fps_f;
        }
    }
    return false; // Run original method with target_fps_f!
}

static bool hook_XNAUnityRunner_ApplyPendingSurfaceRate_prefix(patch_handle_t instance, void** args,
                                                              const patch_method_signature_t* sig_info, void* result) {
    if (instance != PATCH_NULL && s_cached_runner_inst == PATCH_NULL) {
        s_cached_runner_inst = instance;
    }
    return false; // Run original method so surface rate is applied!
}

static void hook_Main_DoUpdate_postfix(patch_handle_t instance, void** args, void* result,
                                       const patch_method_signature_t* sig_info) {
    InterpolationManager::end_keyframe();
}

// 3. Hook Main.SetPlatform60FPS to override vanilla 30/60 FPS locking
static bool hook_Main_SetPlatform60FPS_prefix(patch_handle_t instance, void** args,
                                              const patch_method_signature_t* sig_info, void* result) {
    if (instance != PATCH_NULL && s_cached_game_inst == PATCH_NULL) {
        s_cached_game_inst = instance;
    }
    if (s_initial_fps_applied) {
        LOGI("Main.SetPlatform60FPS intercepted -> enforcing target FPS: %d", g_config.target_fps);
        TimingManager::apply_target_fps(g_config.target_fps);
        return true; // Skip original to prevent vanilla 30/60 override
    }
    return false; // Allow vanilla early boot setup to proceed untouched until s_initial_fps_applied is active
}

// 4. Hook Main.set_Setting_Target60FPS to override vanilla setting changes
static bool hook_Main_set_Setting_Target60FPS_prefix(patch_handle_t instance, void** args,
                                                     const patch_method_signature_t* sig_info, void* result) {
    if (instance != PATCH_NULL && s_cached_game_inst == PATCH_NULL) {
        s_cached_game_inst = instance;
    }
    if (s_initial_fps_applied) {
        LOGI("Main.set_Setting_Target60FPS intercepted -> enforcing target FPS: %d", g_config.target_fps);
        TimingManager::apply_target_fps(g_config.target_fps);
        return true; // Skip original
    }
    return false; // Allow vanilla early boot setup to proceed untouched until s_initial_fps_applied is active
}

// 5. Hook XNAUnityRunner.Awake to apply target FPS once instance is ready
static void hook_XNAUnityRunner_Awake_postfix(patch_handle_t instance, void** args, void* result,
                                              const patch_method_signature_t* sig_info) {
    if (instance != PATCH_NULL) {
        s_cached_runner_inst = instance;
    }
    LOGI("XNAUnityRunner.Awake completed -> applying target FPS: %d", g_config.target_fps);
    s_initial_fps_applied = true;
    TimingManager::apply_target_fps(g_config.target_fps);
}

// 6. Hook Main.LoadSettings to preserve user frame rate choice
static void hook_Main_LoadSettings_postfix(patch_handle_t instance, void** args, void* result,
                                           const patch_method_signature_t* sig_info) {
    if (instance != PATCH_NULL && s_cached_game_inst == PATCH_NULL) {
        s_cached_game_inst = instance;
    }
    if (s_initial_fps_applied) {
        LOGI("Main.LoadSettings completed -> reapplying target FPS: %d", g_config.target_fps);
        TimingManager::apply_target_fps(g_config.target_fps);
    }
}

// 7. Intercept Application.set_targetFrameRate to prevent lowering FPS
static bool hook_Application_set_targetFrameRate_prefix(patch_handle_t instance, void** args,
                                                        const patch_method_signature_t* sig_info, void* result) {
    if (!s_initial_fps_applied) {
        return false; // Allow early Unity boot to proceed untouched
    }
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
    s_runner_type = patchlib_type_get_type("", "XNAUnityRunner");

    if (!s_main_type || !s_game_type) {
        LOGE("Failed to resolve Main or Game types");
        return false;
    }

    // Resolve native Unity Application.set_targetFrameRate icall
    void* resolve_sym = find_il2cpp_symbol("il2cpp_resolve_icall");
    if (resolve_sym) {
        s_il2cpp_resolve_icall = reinterpret_cast<il2cpp_resolve_icall_fn>(resolve_sym);
        s_set_fps_icall = reinterpret_cast<set_targetFrameRate_fn>(
            s_il2cpp_resolve_icall("UnityEngine.Application::set_targetFrameRate"));
        if (!s_set_fps_icall) {
            s_set_fps_icall = reinterpret_cast<set_targetFrameRate_fn>(
                s_il2cpp_resolve_icall("UnityEngine.Application::set_targetFrameRate(System.Int32)"));
        }
        LOGI("Resolved Unity icalls: set_targetFrameRate=%p", s_set_fps_icall);
    }

    // Resolve UnityEngine.Application.set_targetFrameRate via patchlib
    if (s_app_type) {
        s_set_target_fps_method = patchlib_type_get_method_by_param_count(s_app_type, "set_targetFrameRate", 1);
        LOGI("Found Application methods: set_targetFrameRate=%p", s_set_target_fps_method);

        if (s_set_target_fps_method) {
            patchlib_install_prepost_hook(s_set_target_fps_method, hook_Application_set_targetFrameRate_prefix, nullptr);
            LOGI("Hooked Application.set_targetFrameRate successfully");
        }
    }

    // Resolve Game fields
    s_game_high_fps_field = patchlib_type_get_field(s_game_type, "highFrameRate");
    s_game_update_mode_var_field = patchlib_type_get_field(s_game_type, "updateModeVariable");
    s_main_instance_field = patchlib_type_get_field(s_main_type, "instance");
    LOGI("Found Game/Main fields: highFrameRate=%p, updateModeVariable=%p, instance=%p",
         s_game_high_fps_field, s_game_update_mode_var_field, s_main_instance_field);

    // Resolve XNAUnityRunner
    if (s_runner_type) {
        s_runner_inst_field = patchlib_type_get_field(s_runner_type, "_instance");
        s_runner_game_field = patchlib_type_get_field(s_runner_type, "_game");
        s_set_surface_rate_method = patchlib_type_get_method_by_param_count(s_runner_type, "SetAndroidSurfaceRate", 1);
        s_apply_surface_rate_method = patchlib_type_get_method_by_param_count(s_runner_type, "ApplyPendingSurfaceRate", 0);

        s_runner_target_rate_field = patchlib_type_get_field(s_runner_type, "TargetSurfaceRate");
        s_runner_pending_rate_field = patchlib_type_get_field(s_runner_type, "PendingTargetSurfaceRate");
        s_runner_disable_var_field = patchlib_type_get_field(s_runner_type, "DisableVariableUpdate");

        if (s_set_surface_rate_method) {
            patchlib_install_prepost_hook(s_set_surface_rate_method, hook_XNAUnityRunner_SetAndroidSurfaceRate_prefix, nullptr);
            LOGI("Hooked XNAUnityRunner.SetAndroidSurfaceRate successfully");
        }
        if (s_apply_surface_rate_method) {
            patchlib_install_prepost_hook(s_apply_surface_rate_method, hook_XNAUnityRunner_ApplyPendingSurfaceRate_prefix, nullptr);
            LOGI("Hooked XNAUnityRunner.ApplyPendingSurfaceRate successfully");
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

    // Target FPS application is deferred to XNAUnityRunner.Awake (with Game.UnityUpdate fallback)
    // to ensure Unity graphics and surface discovery are initialized safely.
    LOGI("TimingManager initialized successfully (target FPS application deferred to Awake)");

    return true;
}

void TimingManager::apply_target_fps(int fps) {
    if (fps < 30) fps = 30;
    LOGI("=== TimingManager::apply_target_fps(%d) ===", fps);
    s_last_enforced_fps = fps;

    // 1. Set UnityEngine.Application.targetFrameRate
    set_target_framerate(fps);
    LOGI("Applied Application.set_targetFrameRate(%d)", fps);

    // 2. Ensure runner and game state maintain decoupled 60 TPS accumulator logic
    patch_handle_t runner_inst = get_runner_inst();
    if (runner_inst != PATCH_NULL) {
        float target_rate = static_cast<float>(fps);
        if (s_runner_target_rate_field) {
            patchlib_field_set_value(s_runner_target_rate_field, runner_inst, &target_rate);
        }
        if (s_set_surface_rate_method) {
            void* rate_args[] = { &target_rate };
            patchlib_method_invoke_args(s_set_surface_rate_method, runner_inst, nullptr, rate_args);
            LOGI("Invoked XNAUnityRunner.SetAndroidSurfaceRate(%.1f)", target_rate);
        }
        if (s_apply_surface_rate_method) {
            patchlib_method_invoke_args(s_apply_surface_rate_method, runner_inst, nullptr, nullptr);
            LOGI("Invoked XNAUnityRunner.ApplyPendingSurfaceRate()");
        }
        if (s_runner_disable_var_field) {
            bool disable = false;
            patchlib_field_set_value(s_runner_disable_var_field, runner_inst, &disable);
        }
    }

    patch_handle_t game_inst = PATCH_NULL;
    if (s_cached_game_inst != PATCH_NULL) {
        game_inst = s_cached_game_inst;
    } else if (s_main_instance_field) {
        patchlib_field_get_value(s_main_instance_field, PATCH_NULL, &game_inst);
    }
    if (game_inst == PATCH_NULL && runner_inst != PATCH_NULL && s_runner_game_field) {
        patchlib_field_get_value(s_runner_game_field, runner_inst, &game_inst);
    }
    if (game_inst != PATCH_NULL) {
        s_cached_game_inst = game_inst;
        if (s_game_high_fps_field) {
            bool false_val = false;
            patchlib_field_set_value(s_game_high_fps_field, game_inst, &false_val);
        }
        if (s_game_update_mode_var_field) {
            bool true_val = true;
            patchlib_field_set_value(s_game_update_mode_var_field, game_inst, &true_val);
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

bool TimingManager::should_bypass_interpolation() {
    return g_config.target_fps <= 60;
}

float TimingManager::get_partial_tick() {
    if (is_game_paused()) {
        return 1.0f;
    }

    if (should_bypass_interpolation()) {
        return 1.0f;
    }

    auto now = std::chrono::steady_clock::now();
    double elapsed_sec = std::chrono::duration<double>(now - s_last_tick_time).count();
    float partial = static_cast<float>(elapsed_sec * 60.0);

    if (partial < 0.0f) partial = 0.0f;
    if (partial > 1.5f) partial = 1.5f;

    return partial;
}

} // namespace HighFPS
