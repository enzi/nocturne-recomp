#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

struct SceneCamera {
    std::string name;
    float pos[3];
    float m[9];
    float f;
};

struct Scene {
    std::string setKey;
    std::string geoKey;
    std::vector<uint8_t> setBytes;
    std::vector<uint8_t> geoBytes;
    std::vector<SceneCamera> cameras;
    std::vector<std::vector<int>> pvs;
    std::vector<float> verts;
    std::vector<std::pair<int, int>> cellRanges;
};

constexpr int kSceneVertexFloats = 7;
constexpr int kBackdropW = 640;
constexpr int kBackdropH = 480;

void scene_index(const std::filesystem::path& gameDir);
std::vector<std::string> scene_sets();
std::string scene_set_for_camera(const std::string& cameraName);
std::string scene_name(const std::string& setKey);
bool scene_load(const std::string& setKey, Scene& out);
bool scene_backdrop(const std::string& cameraName, std::vector<uint32_t>& rgba);
