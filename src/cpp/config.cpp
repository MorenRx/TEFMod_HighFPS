#include "config.hpp"
#include "json.hpp"
#include "log.hpp"
#include <fstream>
#include <filesystem>

namespace HighFPS {

ModConfig g_config;

void ModConfig::init(const std::string& private_dir) {
    private_dir_path = private_dir;
    std::filesystem::path p(private_dir);
    config_path = (p / "config.json").string();
    load();
}

void ModConfig::load() {
    std::error_code ec;
    if (!config_path.empty() && std::filesystem::exists(config_path, ec) && !ec) {
        try {
            std::ifstream file(config_path);
            if (file.is_open()) {
                nlohmann::json root;
                file >> root;
                // Backwards-compatible: check if wrapped under "values"
                const nlohmann::json& values = (root.contains("values") && root["values"].is_object())
                                               ? root["values"]
                                               : root;

                if (values.contains("target_fps")) {
                    if (values["target_fps"].is_number()) {
                        target_fps = values["target_fps"].get<int>();
                    } else if (values["target_fps"].is_string()) {
                        target_fps = std::stoi(values["target_fps"].get<std::string>());
                    }
                }
            }
        } catch (const std::exception& e) {
            LOGW("Failed to parse config.json: %s", e.what());
        }
    }

    // Validate target_fps against allowed tiers (30, 60, 90, 120, 144, 240)
    bool valid_fps = false;
    for (int opt : FPS_OPTIONS) {
        if (target_fps == opt) {
            valid_fps = true;
            break;
        }
    }
    if (!valid_fps) {
        target_fps = 60; // Default to 60 if invalid or legacy -1
    }

    LOGI("Active settings (target_fps: %d)", target_fps);
}

void ModConfig::save() {
    if (config_path.empty()) return;

    try {
        nlohmann::json root;
        root["target_fps"] = target_fps;

        std::filesystem::path p(config_path);
        if (p.has_parent_path()) {
            std::error_code ec;
            std::filesystem::create_directories(p.parent_path(), ec);
        }

        std::ofstream file(config_path);
        if (file.is_open()) {
            file << root.dump(2);
            LOGI("Saved settings to %s (target_fps: %d)", config_path.c_str(), target_fps);
        }
    } catch (const std::exception& e) {
        LOGE("Failed to save config: %s", e.what());
    }
}

int ModConfig::get_fps_index() const {
    for (int i = 0; i < FPS_COUNT; ++i) {
        if (FPS_OPTIONS[i] == target_fps) {
            return i;
        }
    }
    return 1; // Default to 60 (index 1)
}

void ModConfig::set_fps_by_index(int index) {
    if (index >= 0 && index < FPS_COUNT) {
        target_fps = FPS_OPTIONS[index];
    }
}

const char* ModConfig::get_fps_label() const {
    int idx = get_fps_index();
    return FPS_LABELS[idx];
}

void ModConfig::cycle_fps() {
    int next_idx = (get_fps_index() + 1) % FPS_COUNT;
    set_fps_by_index(next_idx);
    save();
}

} // namespace HighFPS
