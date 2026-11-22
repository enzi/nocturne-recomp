#include "bake.h"

#include "xatlas/xatlas.h"

#include "stb/stb_image.h"
#include "stb/stb_image_write.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <unordered_map>

namespace fs = std::filesystem;

namespace {

constexpr uint32_t kVersion = 8;
constexpr float kTexelsPerUnit = 32.0f;
constexpr int kDensityGroups = 4;
constexpr int kPageSize = 4096;
constexpr int kPadding = 4;
constexpr int kVertexFloats = 7;

struct KeyHash {
    size_t operator()(const std::array<long long, 3>& k) const {
        return std::hash<long long>()(k[0] * 73856093LL ^ k[1] * 19349663LL ^ k[2] * 83492791LL);
    }
};

}

uint64_t bake_hash(const std::vector<uint8_t>& geo, const std::vector<uint8_t>& set) {
    uint64_t h = 1469598103934665603ULL;
    auto mix = [&](const uint8_t* p, size_t n) {
        for (size_t i = 0; i < n; i++) {
            h = (h ^ p[i]) * 1099511628211ULL;
        }
    };
    uint32_t params[3] = {kVersion, uint32_t(kTexelsPerUnit), uint32_t(kPageSize)};
    mix(reinterpret_cast<const uint8_t*>(params), sizeof params);
    mix(geo.data(), geo.size());
    mix(set.data(), set.size());
    return h;
}

bool bake_unwrap(const BakeInput& in, BakeAtlas& out) {
    size_t faces = in.verts.size() / (3 * kVertexFloats);
    struct Group {
        float scale = 1.0f;
        std::vector<size_t> faces;
        std::vector<float> positions;
        std::vector<uint32_t> indices;
    };
    Group groups[kDensityGroups];
    for (int k = 0; k < kDensityGroups; k++) {
        groups[k].scale = 1.0f / float(1 << k);
    }
    size_t seen = 0;
    for (size_t f = 0; f < faces; f++) {
        float density = f < in.faceDensity.size() ? in.faceDensity[f] : 0.0f;
        int k = kDensityGroups - 1;
        while (density > 0.0f && k > 0 && kTexelsPerUnit * groups[k].scale < density) {
            k--;
        }
        groups[k].faces.push_back(f);
        seen += density > 0.0f;
    }
    xatlas::Atlas* atlas = xatlas::Create();
    std::vector<Group*> added;
    for (Group& g : groups) {
        if (g.faces.empty()) {
            continue;
        }
        std::unordered_map<std::array<long long, 3>, uint32_t, KeyHash> welded;
        for (size_t f : g.faces) {
            for (int k = 0; k < 3; k++) {
                const float* p = in.verts.data() + (f * 3 + size_t(k)) * kVertexFloats;
                std::array<long long, 3> key = {std::llround(p[0] * 1024.0f), std::llround(p[1] * 1024.0f), std::llround(p[2] * 1024.0f)};
                auto [it, inserted] = welded.emplace(key, uint32_t(g.positions.size() / 3));
                if (inserted) {
                    g.positions.insert(g.positions.end(), {p[0] * g.scale, p[1] * g.scale, p[2] * g.scale});
                }
                g.indices.push_back(it->second);
            }
        }
        xatlas::MeshDecl decl;
        decl.vertexPositionData = g.positions.data();
        decl.vertexCount = uint32_t(g.positions.size() / 3);
        decl.vertexPositionStride = 12;
        decl.indexData = g.indices.data();
        decl.indexCount = uint32_t(g.indices.size());
        decl.indexFormat = xatlas::IndexFormat::UInt32;
        xatlas::AddMeshError err = xatlas::AddMesh(atlas, decl);
        if (err != xatlas::AddMeshError::Success) {
            std::fprintf(stderr, "[bake] xatlas AddMesh failed: %s\n", xatlas::StringForEnum(err));
            xatlas::Destroy(atlas);
            return false;
        }
        added.push_back(&g);
    }
    xatlas::PackOptions pack;
    pack.texelsPerUnit = kTexelsPerUnit;
    pack.resolution = kPageSize;
    pack.padding = kPadding;
    pack.bilinear = true;
    xatlas::Generate(atlas, xatlas::ChartOptions(), pack);
    if (atlas->meshCount != added.size() || atlas->atlasCount == 0 || atlas->width != uint32_t(kPageSize)) {
        std::fprintf(stderr, "[bake] unexpected xatlas output: %u meshes, %u pages, width %u\n", atlas->meshCount, atlas->atlasCount, atlas->width);
        xatlas::Destroy(atlas);
        return false;
    }
    out.pageSize = kPageSize;
    out.pages = int(atlas->atlasCount);
    out.uvPage.assign(faces * 9, -1.0f);
    for (size_t m = 0; m < added.size(); m++) {
        const xatlas::Mesh& mesh = atlas->meshes[m];
        const Group& g = *added[m];
        if (mesh.indexCount != g.indices.size()) {
            std::fprintf(stderr, "[bake] xatlas changed the face count of mesh %zu\n", m);
            xatlas::Destroy(atlas);
            return false;
        }
        for (size_t i = 0; i < g.indices.size(); i++) {
            const xatlas::Vertex& v = mesh.vertexArray[mesh.indexArray[i]];
            float* uv = out.uvPage.data() + (g.faces[i / 3] * 3 + i % 3) * 3;
            uv[0] = v.uv[0];
            uv[1] = v.uv[1];
            uv[2] = v.atlasIndex < 0 ? -1.0f : float(v.atlasIndex);
        }
    }
    std::fprintf(stderr, "[bake] %zu of %zu faces seen, faces at 32/16/8/4 texels per unit: %zu/%zu/%zu/%zu, %u charts on %u pages of %d, utilization", seen, faces, groups[0].faces.size(), groups[1].faces.size(), groups[2].faces.size(), groups[3].faces.size(), atlas->chartCount, atlas->atlasCount, kPageSize);
    for (uint32_t i = 0; i < atlas->atlasCount; i++) {
        std::fprintf(stderr, " %.0f%%", atlas->utilization[i] * 100.0f);
    }
    std::fprintf(stderr, "\n");
    xatlas::Destroy(atlas);
    return true;
}

void bake_fill(std::vector<uint32_t>& rgba, int size) {
    std::vector<std::vector<uint32_t>> levels;
    std::vector<int> sizes = {size};
    for (int s = size; s > 1; s /= 2) {
        const std::vector<uint32_t>& fine = levels.empty() ? rgba : levels.back();
        int h = s / 2;
        std::vector<uint32_t> coarse(size_t(h) * h, 0);
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < h; x++) {
                uint32_t r = 0, g = 0, b = 0, n = 0;
                for (int k = 0; k < 4; k++) {
                    uint32_t p = fine[size_t(2 * y + k / 2) * size_t(s) + size_t(2 * x + k % 2)];
                    if (p >> 24) {
                        r += p & 0xff;
                        g += (p >> 8) & 0xff;
                        b += (p >> 16) & 0xff;
                        n++;
                    }
                }
                if (n) {
                    coarse[size_t(y) * size_t(h) + size_t(x)] = 0xff000000u | ((b / n) << 16) | ((g / n) << 8) | (r / n);
                }
            }
        }
        levels.push_back(std::move(coarse));
        sizes.push_back(h);
    }
    for (size_t l = levels.size(); l > 0; l--) {
        const std::vector<uint32_t>& parent = levels[l - 1];
        std::vector<uint32_t>& child = l == 1 ? rgba : levels[l - 2];
        int cs = sizes[l - 1];
        int ps = sizes[l];
        for (int y = 0; y < cs; y++) {
            for (int x = 0; x < cs; x++) {
                uint32_t& p = child[size_t(y) * size_t(cs) + size_t(x)];
                if (p >> 24) {
                    continue;
                }
                float fx = (float(x) + 0.5f) * 0.5f - 0.5f;
                float fy = (float(y) + 0.5f) * 0.5f - 0.5f;
                int x0 = int(std::floor(fx)), y0 = int(std::floor(fy));
                float tx = fx - float(x0), ty = fy - float(y0);
                float sum[3] = {0, 0, 0}, wsum = 0;
                for (int k = 0; k < 4; k++) {
                    int px = std::clamp(x0 + k % 2, 0, ps - 1);
                    int py = std::clamp(y0 + k / 2, 0, ps - 1);
                    uint32_t q = parent[size_t(py) * size_t(ps) + size_t(px)];
                    float w = (k % 2 ? tx : 1 - tx) * (k / 2 ? ty : 1 - ty);
                    if (q >> 24 && w > 0) {
                        sum[0] += w * float(q & 0xff);
                        sum[1] += w * float((q >> 8) & 0xff);
                        sum[2] += w * float((q >> 16) & 0xff);
                        wsum += w;
                    }
                }
                if (wsum > 0) {
                    p = 0xff000000u | (uint32_t(sum[2] / wsum + 0.5f) << 16) | (uint32_t(sum[1] / wsum + 0.5f) << 8) | uint32_t(sum[0] / wsum + 0.5f);
                }
            }
        }
    }
}

bool bake_save(const fs::path& dir, uint64_t hash, const BakeAtlas& atlas, const std::vector<std::vector<uint32_t>>& pages) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    for (int p = 0; p < atlas.pages; p++) {
        fs::path path = dir / ("page" + std::to_string(p) + ".png");
        if (!stbi_write_png(path.string().c_str(), atlas.pageSize, atlas.pageSize, 4, pages[size_t(p)].data(), atlas.pageSize * 4)) {
            std::fprintf(stderr, "[bake] cannot write %s\n", path.string().c_str());
            return false;
        }
    }
    int stale = atlas.pages;
    while (fs::remove(dir / ("page" + std::to_string(stale) + ".png"), ec)) {
        stale++;
    }
    std::ofstream out(dir / "mesh.bin", std::ios::binary);
    uint32_t header[5] = {kVersion, uint32_t(atlas.pageSize), uint32_t(atlas.pages), uint32_t(atlas.uvPage.size() / 3), 0};
    out.write("NBAK", 4);
    out.write(reinterpret_cast<const char*>(&hash), sizeof hash);
    out.write(reinterpret_cast<const char*>(header), sizeof header);
    out.write(reinterpret_cast<const char*>(atlas.uvPage.data()), std::streamsize(atlas.uvPage.size() * sizeof(float)));
    return bool(out);
}

bool bake_load(const fs::path& dir, uint64_t hash, size_t vertexCount, BakeAtlas& atlas, std::vector<std::vector<uint32_t>>& pages) {
    std::ifstream in(dir / "mesh.bin", std::ios::binary);
    char magic[4];
    uint64_t fileHash = 0;
    uint32_t header[5];
    if (!in.read(magic, 4) || std::memcmp(magic, "NBAK", 4) != 0 || !in.read(reinterpret_cast<char*>(&fileHash), sizeof fileHash) || !in.read(reinterpret_cast<char*>(header), sizeof header)) {
        return false;
    }
    if (fileHash != hash || header[0] != kVersion || header[3] != vertexCount || header[2] == 0) {
        std::fprintf(stderr, "[bake] cache in %s is stale\n", dir.string().c_str());
        return false;
    }
    atlas.pageSize = int(header[1]);
    atlas.pages = int(header[2]);
    atlas.uvPage.resize(vertexCount * 3);
    if (!in.read(reinterpret_cast<char*>(atlas.uvPage.data()), std::streamsize(atlas.uvPage.size() * sizeof(float)))) {
        return false;
    }
    pages.assign(size_t(atlas.pages), {});
    for (int p = 0; p < atlas.pages; p++) {
        fs::path path = dir / ("page" + std::to_string(p) + ".png");
        int w = 0, h = 0, n = 0;
        stbi_uc* data = stbi_load(path.string().c_str(), &w, &h, &n, 4);
        if (!data || w != atlas.pageSize || h != atlas.pageSize) {
            std::fprintf(stderr, "[bake] cannot read %s\n", path.string().c_str());
            stbi_image_free(data);
            return false;
        }
        pages[size_t(p)].resize(size_t(w) * size_t(h));
        std::memcpy(pages[size_t(p)].data(), data, pages[size_t(p)].size() * 4);
        stbi_image_free(data);
    }
    return true;
}
