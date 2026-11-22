#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

struct BakeInput {
    std::vector<float> verts;
    std::vector<float> faceDensity;
};

struct BakeAtlas {
    int pageSize = 0;
    int pages = 0;
    std::vector<float> uvPage;
};

uint64_t bake_hash(const std::vector<uint8_t>& geo, const std::vector<uint8_t>& set);
bool bake_unwrap(const BakeInput& in, BakeAtlas& out);
void bake_fill(std::vector<uint32_t>& rgba, int size);
bool bake_save(const std::filesystem::path& dir, uint64_t hash, const BakeAtlas& atlas, const std::vector<std::vector<uint32_t>>& pages);
bool bake_load(const std::filesystem::path& dir, uint64_t hash, size_t vertexCount, BakeAtlas& atlas, std::vector<std::vector<uint32_t>>& pages);
