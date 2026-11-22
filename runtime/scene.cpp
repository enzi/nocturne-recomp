#include "scene.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>

namespace fs = std::filesystem;

namespace {

struct PodEntry {
    fs::path pod;
    uint32_t offset;
    uint32_t size;
};

std::map<std::string, PodEntry> g_files;
std::map<std::string, std::string> g_cameraToSet;

std::string upper(std::string s) {
    for (char& ch : s) {
        ch = char(std::toupper(static_cast<unsigned char>(ch)));
    }
    return s;
}

std::vector<uint8_t> readFile(const std::string& key) {
    auto it = g_files.find(upper(key));
    if (it == g_files.end()) {
        return {};
    }
    std::ifstream in(it->second.pod, std::ios::binary);
    std::vector<uint8_t> buf(it->second.size);
    in.seekg(it->second.offset);
    in.read(reinterpret_cast<char*>(buf.data()), std::streamsize(buf.size()));
    return buf;
}

void indexPod(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    char header[0x60];
    if (!in.read(header, sizeof header) || std::memcmp(header, "POD2", 4) != 0) {
        return;
    }
    uint32_t count;
    std::memcpy(&count, header + 0x58, 4);
    std::vector<uint32_t> table(size_t(count) * 5);
    in.read(reinterpret_cast<char*>(table.data()), std::streamsize(table.size() * 4));
    uint32_t namesSize = 0;
    for (uint32_t i = 0; i < count; i++) {
        namesSize = std::max(namesSize, table[i * 5] + 256);
    }
    std::vector<char> names(namesSize + 1, 0);
    in.read(names.data(), namesSize);
    for (uint32_t i = 0; i < count; i++) {
        std::string name(names.data() + table[i * 5]);
        g_files[upper(name)] = {path, table[i * 5 + 2], table[i * 5 + 1]};
    }
}

std::vector<std::string> lines(const std::vector<uint8_t>& data) {
    std::vector<std::string> out;
    std::stringstream ss(std::string(data.begin(), data.end()));
    std::string line;
    while (std::getline(ss, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
            line.pop_back();
        }
        size_t start = line.find_first_not_of(" \t");
        out.push_back(start == std::string::npos ? std::string() : line.substr(start));
    }
    return out;
}

std::vector<float> parseFloats(const std::string& s) {
    std::vector<float> v;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ',')) {
        v.push_back(std::stof(item));
    }
    return v;
}

std::vector<float> loadGeometry(const std::vector<uint8_t>& g, std::vector<std::pair<int, int>>& cellRanges) {
    std::vector<float> out;
    auto i32 = [&](size_t o) { int32_t v; std::memcpy(&v, g.data() + o, 4); return v; };
    auto f32 = [&](size_t o) { float v; std::memcpy(&v, g.data() + o, 4); return v; };
    if (g.size() < 0x34) {
        return out;
    }
    int cells = i32(4) * i32(8) * i32(12);
    size_t o = 0x34;
    for (int c = 0; c < cells && o + 32 <= g.size(); c++) {
        int nv = i32(o + 24);
        int nf = i32(o + 28);
        size_t verts = o + 32;
        o = verts + size_t(nv) * 12;
        int first = int(out.size() / kSceneVertexFloats);
        for (int f = 0; f < nf; f++) {
            float nx = -f32(o + 12), ny = -f32(o + 16), nz = -f32(o + 20);
            for (int k = 0; k < 3; k++) {
                size_t v = verts + size_t(i32(o + 4 * k)) * 12;
                out.insert(out.end(), {f32(v), f32(v + 4), f32(v + 8), nx, ny, nz, float(c)});
            }
            o += 32;
        }
        cellRanges.emplace_back(first, int(out.size() / kSceneVertexFloats) - first);
        if (nf) {
            o += size_t(nf) + 64;
        }
    }
    return out;
}

std::vector<std::vector<int>> parsePvs(const std::vector<std::string>& ls, size_t cameraCount) {
    std::vector<std::vector<int>> pvs;
    size_t i = 0;
    while (i < ls.size() && ls[i] != "PVS list") {
        i++;
    }
    i++;
    while (pvs.size() < cameraCount && i < ls.size()) {
        int n = std::stoi(ls[i++]);
        std::vector<int> cells;
        for (int k = 0; k < n && i < ls.size(); k++) {
            cells.push_back(std::stoi(ls[i++]));
        }
        pvs.push_back(cells);
    }
    if (pvs.size() != cameraCount) {
        pvs.clear();
    }
    return pvs;
}

}

void scene_index(const fs::path& gameDir) {
    g_files.clear();
    g_cameraToSet.clear();
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(gameDir, ec)) {
        if (upper(e.path().extension().string()) == ".POD") {
            indexPod(e.path());
        }
    }
    for (const auto& [key, entry] : g_files) {
        if (key.size() < 4 || key.compare(key.size() - 4, 4, ".SET") != 0) {
            continue;
        }
        std::vector<std::string> ls = lines(readFile(key));
        for (size_t i = 0; i + 1 < ls.size(); i++) {
            if (ls[i].rfind("-- camera name", 0) == 0) {
                g_cameraToSet[upper(ls[i + 1])] = key;
            }
        }
    }
    std::fprintf(stderr, "[scene] indexed %zu files, %zu cameras\n", g_files.size(), g_cameraToSet.size());
}

std::vector<std::string> scene_sets() {
    std::vector<std::string> out;
    for (const auto& [camera, set] : g_cameraToSet) {
        if (std::find(out.begin(), out.end(), set) == out.end()) {
            out.push_back(set);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::string scene_set_for_camera(const std::string& cameraName) {
    auto it = g_cameraToSet.find(upper(cameraName));
    return it == g_cameraToSet.end() ? std::string() : it->second;
}

std::string scene_name(const std::string& setKey) {
    std::string name = setKey.substr(setKey.find_last_of('\\') + 1);
    return name.substr(0, name.find('.'));
}

bool scene_load(const std::string& setKey, Scene& out) {
    out = Scene{};
    out.setKey = setKey;
    out.setBytes = readFile(setKey);
    std::vector<std::string> ls = lines(out.setBytes);
    std::string geoName;
    for (const std::string& l : ls) {
        if (l.size() > 4 && upper(l.substr(l.size() - 4)) == ".GEO") {
            geoName = upper(l);
            break;
        }
    }
    for (size_t i = 0; i + 1 < ls.size(); i++) {
        if (ls[i].rfind("-- camera name", 0) != 0) {
            continue;
        }
        SceneCamera cam{};
        cam.name = upper(ls[i + 1]);
        float fov = 30.0f;
        for (size_t k = i + 2; k < ls.size() && ls[k].rfind("-- camera name", 0) != 0; k++) {
            if (ls[k] == "pos" && k + 1 < ls.size()) {
                std::vector<float> p = parseFloats(ls[k + 1]);
                std::copy_n(p.begin(), 3, cam.pos);
            } else if (ls[k] == "fov" && k + 1 < ls.size()) {
                fov = std::stof(ls[k + 1]);
            } else if (ls[k] == "vmat" && k + 3 < ls.size()) {
                for (int r = 0; r < 3; r++) {
                    std::vector<float> row = parseFloats(ls[k + 1 + r]);
                    std::copy_n(row.begin(), 3, cam.m + 3 * r);
                }
                break;
            }
        }
        cam.f = 319.0f * fov / 18.0f;
        out.cameras.push_back(cam);
    }
    for (const auto& [key, e] : g_files) {
        if (!geoName.empty() && key.size() >= geoName.size() + 1 && key.compare(key.size() - geoName.size() - 1, std::string::npos, "\\" + geoName) == 0) {
            out.geoKey = key;
            break;
        }
    }
    out.geoBytes = readFile(out.geoKey);
    out.verts = loadGeometry(out.geoBytes, out.cellRanges);
    out.pvs = parsePvs(ls, out.cameras.size());
    std::fprintf(stderr, "[scene] %s: %s, %zu triangles, %zu cameras, PVS %s\n", setKey.c_str(), out.geoKey.c_str(), out.verts.size() / (3 * kSceneVertexFloats), out.cameras.size(), out.pvs.empty() ? "missing" : "loaded");
    return !out.verts.empty() && !out.cameras.empty();
}

bool scene_backdrop(const std::string& cameraName, std::vector<uint32_t>& rgba) {
    std::vector<uint8_t> raw = readFile("BACKDROP\\" + cameraName + ".RAW");
    std::vector<uint8_t> pal = readFile("BACKDROP\\" + cameraName + ".ACT");
    rgba.assign(size_t(kBackdropW) * kBackdropH, 0xff202020u);
    if (raw.size() < rgba.size() || pal.size() < 768) {
        std::fprintf(stderr, "[scene] missing backdrop for %s\n", cameraName.c_str());
        return false;
    }
    for (size_t p = 0; p < rgba.size(); p++) {
        const uint8_t* c = pal.data() + raw[p] * 3;
        rgba[p] = 0xff000000u | (uint32_t(c[2]) << 16) | (uint32_t(c[1]) << 8) | c[0];
    }
    return true;
}
