#pragma once

#include <string>
#include <vector>

namespace HighFPS {

struct ModConfig {
    static constexpr int FPS_OPTIONS[] = { 30, 60, 120, 240 };
    static constexpr const char* FPS_LABELS[] = { "30 FPS", "60 FPS", "120 FPS", "240 FPS" };
    static constexpr int FPS_COUNT = sizeof(FPS_OPTIONS) / sizeof(FPS_OPTIONS[0]);

    int target_fps = 60;

    std::string private_dir_path;
    std::string config_path;

    void init(const std::string& private_dir);
    void load();
    void save();

    int get_fps_index() const;
    void set_fps_by_index(int index);
    const char* get_fps_label() const;
    void cycle_fps();
};

extern ModConfig g_config;

} // namespace HighFPS
