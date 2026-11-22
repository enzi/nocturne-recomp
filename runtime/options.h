#pragma once

#include <filesystem>

struct Options {
    float batteryDrainScale = 0.0f;
    bool texturedNightVision = false;
};

extern Options g_options;

void options_load(const std::filesystem::path& gameDir);
void options_save();
void options_cycle_battery_drain();
void options_toggle_textured_night_vision();
