#include "funcs.h"
#include "hle.h"
#include "options.h"
#include "renderer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace {

constexpr uint32_t kActiveCamera = 0x01fb64d8;
constexpr uint32_t kCameraSize = 0x11e8;
constexpr uint32_t kHardwareFlag = 0x01c00594;
constexpr uint32_t kScenePtr = 0x5bd088;
constexpr uint32_t kRendererPtr = 0x5ad444;
constexpr uint32_t kTextureMode = 0x6ae280;
constexpr uint32_t kDynamicLightCount = 0x01fb82a8;
constexpr uint32_t kDynamicLights = 0x01fb82ac;
constexpr uint32_t kFogDisabled = 0x0140cfa8;
constexpr float kFirstPersonHalfAngle = 40.0f;
constexpr float kPi = 3.14159265358979f;

bool g_firstPersonPass = false;

float rdf(uint8_t* m, uint32_t a) {
    return rd<float>(m, a);
}

bool texturedGoggleView(Ctx* c, uint8_t* m, uint32_t light) {
    World3dView view{};
    for (int i = 0; i < 3; i++) {
        view.pos[i] = rdf(m, light + 0x104 + 4 * i);
    }
    for (int i = 0; i < 9; i++) {
        view.m[i] = rdf(m, light + 0x110 + 4 * i);
    }
    view.f = 320.0f / std::tan(kFirstPersonHalfAngle * kPi / 180.0f);
    if (!renderer_world3d_begin(view)) {
        return false;
    }
    std::vector<uint8_t> camera(m + kActiveCamera, m + kActiveCamera + kCameraSize);
    std::memcpy(m + kActiveCamera + 0x104, m + light + 0x104, 12);
    std::memcpy(m + kActiveCamera + 0x110, m + light + 0x110, 36);
    wr<float>(m, kActiveCamera + 0x138, view.f * 18.0f / 319.0f);
    uint32_t hardware = rd32(m, kHardwareFlag);
    wr32(m, kHardwareFlag, 1);
    uint32_t renderer = rd32(m, kRendererPtr);
    uint32_t textureMode = rd32(m, kTextureMode);
    uint32_t untextured = rd32(m, renderer + 0x0c);
    call_guest(c, m, 0x460d90, {renderer, 1});
    uint32_t scene = rd32(m, kScenePtr);
    call_guest(c, m, 0x440070, {kActiveCamera, 1});
    if (rd32(m, scene + 0x161240)) {
        call_guest(c, m, 0x463330, {rd32(m, 0x5af3b4), scene + 0x161244, rd32(m, scene + 0x161264)});
    }
    uint32_t fogDisabled = rd32(m, kFogDisabled);
    wr32(m, kFogDisabled, 1);
    g_firstPersonPass = true;
    call_guest(c, m, 0x50c420, {scene, 0xc});
    call_guest(c, m, 0x509790, {scene});
    call_guest(c, m, 0x4af6f0, {rd32(m, 0x5b8404)});
    uint32_t effects = rd32(m, 0x5bfefc);
    if (rd32(m, effects)) {
        call_guest(c, m, 0x54fb70, {effects, 0});
    }
    call_guest(c, m, 0x509930, {scene});
    if (rd32(m, scene + 0x15a8c0)) {
        call_guest(c, m, 0x5481d0, {rd32(m, 0x5bfe54), 0});
    }
    call_guest(c, m, 0x48a400, {rd32(m, 0x5b6e30)});
    g_firstPersonPass = false;
    wr32(m, kFogDisabled, fogDisabled);
    call_guest(c, m, 0x440800, {kActiveCamera, 1});
    wr32(m, renderer + 0x0c, untextured);
    wr32(m, kTextureMode, textureMode);
    wr32(m, kHardwareFlag, hardware);
    std::memcpy(m + kActiveCamera, camera.data(), camera.size());
    renderer_world3d_end();
    return true;
}

}

void f_00557660(Ctx* __restrict c, uint8_t* __restrict m) {
    constexpr uint32_t kQueryPerformanceFrequency = 0x574574;
    constexpr uint32_t kQueryPerformanceCounter = 0x574570;
    call_guest(c, m, rd32(m, kQueryPerformanceFrequency), {0x2e05cc0});
    long double freq = (long double)(int32_t(rd32(m, 0x2e05cc4))) * 4294967296.0L + rd32(m, 0x2e05cc0);
    wrf64(m, 0x2e05cc8, freq);
    call_guest(c, m, rd32(m, kQueryPerformanceCounter), {0x2e05cb8});
    wr32(m, 0x1c01950, 1000);
    c->eax = 1000;
    c->esp += 4;
}

void f_004c1100(Ctx* __restrict c, uint8_t* __restrict m) {
    uint32_t inventory = rd32(m, c->esp + 4);
    float before = inventory ? rdf(m, inventory) : 0.0f;
    f_004c1100_orig(c, m);
    float scale = g_options.batteryDrainScale;
    if (inventory && scale != 1.0f) {
        float after = rdf(m, inventory);
        if (after < before) {
            wr<float>(m, inventory, before - (before - after) * scale);
        }
    }
}

void f_005093b0(Ctx* __restrict c, uint8_t* __restrict m) {
    if (world3d_active()) {
        std::vector<uint32_t> lights;
        for (uint32_t i = 0, n = std::min<uint32_t>(rd32(m, kDynamicLightCount), 4); i < n; i++) {
            uint32_t light = rd32(m, kDynamicLights + 4 * i);
            if (light && rd32(m, light + 0x1cb4) && rd32(m, light + 0x1cd0)) {
                lights.push_back(light);
            }
        }
        world3d_capture_dynamic_lights(lights);
    }
    f_005093b0_orig(c, m);
}

void f_0050d5e0(Ctx* __restrict c, uint8_t* __restrict m) {
    if (g_firstPersonPass) {
        c->eax = 1;
        c->esp += 4;
        return;
    }
    f_0050d5e0_orig(c, m);
}

void f_00451ef0(Ctx* __restrict c, uint8_t* __restrict m) {
    if (world3d_active() && texturedGoggleView(c, m, rd32(m, c->esp + 4))) {
        c->esp += 4;
        return;
    }
    f_00451ef0_orig(c, m);
}
