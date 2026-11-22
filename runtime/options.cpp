#include "options.h"

#include <cstdio>
#include <fstream>
#include <string>

Options g_options;

namespace {

std::filesystem::path g_path;

}

void options_load(const std::filesystem::path& gameDir) {
    g_path = gameDir / "nocturne-recomp.ini";
    std::ifstream in(g_path);
    if (!in) {
        options_save();
        return;
    }
    std::string line;
    while (std::getline(in, line)) {
        size_t eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        std::string key = line.substr(0, eq);
        std::string value = line.substr(eq + 1);
        if (key == "batteryDrainScale") {
            g_options.batteryDrainScale = std::stof(value);
        } else if (key == "texturedNightVision") {
            g_options.texturedNightVision = std::stoi(value) != 0;
        }
    }
    std::fprintf(stderr, "[options] batteryDrainScale=%.2f texturedNightVision=%d\n", g_options.batteryDrainScale, g_options.texturedNightVision);
}

void options_save() {
    std::ofstream out(g_path);
    out << "[Recomp]\n";
    out << "batteryDrainScale=" << g_options.batteryDrainScale << "\n";
    out << "texturedNightVision=" << (g_options.texturedNightVision ? 1 : 0) << "\n";
}

void options_toggle_textured_night_vision() {
    g_options.texturedNightVision = !g_options.texturedNightVision;
    std::fprintf(stderr, "[options] textured night vision %s\n", g_options.texturedNightVision ? "on" : "off");
    options_save();
}

void options_cycle_battery_drain() {
    static const float steps[] = {1.0f, 0.5f, 0.25f, 0.0f};
    size_t next = 0;
    for (size_t i = 0; i < 4; i++) {
        if (g_options.batteryDrainScale >= steps[i] - 0.001f) {
            next = (i + 1) % 4;
            break;
        }
    }
    g_options.batteryDrainScale = steps[next];
    std::fprintf(stderr, "[options] battery drain %.0f%%\n", g_options.batteryDrainScale * 100);
    options_save();
}
