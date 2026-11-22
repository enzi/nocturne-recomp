#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

constexpr float kWorld3dFarScale = 16.0f;

struct World3dView {
    float pos[3];
    float m[9];
    float f;
    float zfar;
    float farScale;
    bool linearDepth;
};

void world3d_init(const std::filesystem::path& gameDir);
bool world3d_active();
void world3d_capture_dynamic_lights(const std::vector<uint32_t>& lights);
bool world3d_render(uint32_t fbo, int width, int height, const World3dView& view);
