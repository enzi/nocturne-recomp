#include "bake.h"
#include "projection.h"
#include "scene.h"

#include <SDL3/SDL.h>

#include "stb/stb_image_write.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

const char* kEvalVS = R"(#version 330 core
layout(location = 0) in vec3 pos;
layout(location = 1) in vec3 normal;
layout(location = 3) in vec3 uvPage;
uniform vec3 camPos;
uniform mat3 camRot;
uniform float camF;
out vec3 worldPos;
out vec3 worldNormal;
out vec3 atlasUv;
out float facing;
void main() {
    vec3 d = pos - camPos;
    vec3 c = vec3(dot(camRot[0], d), dot(camRot[1], d), dot(camRot[2], d));
    const float near = 0.5;
    const float far = 4000.0;
    float zc = c.z * (far + near) / (far - near) - 2.0 * far * near / (far - near);
    gl_Position = vec4(camF * c.x / 320.0, -camF * c.y / 240.0, zc, c.z);
    worldPos = pos;
    worldNormal = normal;
    atlasUv = uvPage;
    facing = dot(normal, -d);
}
)";

const char* kEvalFS = R"(
in vec3 worldPos;
in vec3 worldNormal;
in vec3 atlasUv;
in float facing;
uniform int exclude;
uniform int mode;
uniform float pageSize;
uniform sampler2DArray atlas;
out vec4 color;
void main() {
    if (facing <= 0.0) {
        if (mode == 2) {
            color = vec4(0.0, 0.0, 0.0, 32.0 / 255.0);
            return;
        }
        discard;
    }
    if (mode != 0) {
        float page = floor(atlasUv.z + 0.5);
        if (page < 0.0) {
            color = vec4(0.0, 0.0, 0.0, mode == 2 ? 64.0 / 255.0 : 0.5);
            return;
        }
        vec4 t = texture(atlas, vec3(atlasUv.xy / pageSize, page));
        color = t.a < 0.75 ? (mode == 2 ? vec4(t.rgb, 128.0 / 255.0) : vec4(0.0, 0.0, 0.0, 0.5)) : vec4(t.rgb, 1.0);
        return;
    }
    vec3 c;
    int n = projectColor(worldPos, normalize(worldNormal), exclude, c);
    color = n > 0 ? vec4(c, 1.0) : vec4(0.0, 0.0, 0.0, 0.5);
}
)";

struct Options {
    fs::path gameDir;
    fs::path outDir = "data/export";
    bool bake = true;
    bool eval = false;
    bool exportScene = false;
    int images = 6;
    std::vector<std::string> levels;
};

struct CameraScore {
    int camera;
    double coverage;
    double error;
    size_t geometry;
};

struct ViewRenderer {
    GLuint program = 0;
    GLuint fbo = 0;
    GLuint color = 0;
    GLuint depth = 0;

    ViewRenderer(const ProjectionGpu& gpu, GLuint atlasTex, int pageSize) {
        static GLuint linked = 0;
        if (!linked) {
            std::string fs = std::string("#version 330 core\n") + kProjectionGlsl + kEvalFS;
            linked = projection_link(kEvalVS, fs.c_str());
        }
        program = linked;
        glGenTextures(1, &color);
        glBindTexture(GL_TEXTURE_2D, color);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kBackdropW, kBackdropH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glGenRenderbuffers(1, &depth);
        glBindRenderbuffer(GL_RENDERBUFFER, depth);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT32F, kBackdropW, kBackdropH);
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth);
        glViewport(0, 0, kBackdropW, kBackdropH);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LESS);
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
        glDisable(GL_SCISSOR_TEST);
        glUseProgram(program);
        projection_bind(gpu, program, 0);
        glUniform1i(glGetUniformLocation(program, "atlas"), 3);
        glUniform1f(glGetUniformLocation(program, "pageSize"), float(pageSize));
        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D_ARRAY, atlasTex);
        glActiveTexture(GL_TEXTURE0);
        glBindVertexArray(gpu.vao);
    }

    ~ViewRenderer() {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &fbo);
        glDeleteRenderbuffers(1, &depth);
        glDeleteTextures(1, &color);
    }

    void render(const ProjectionGpu& gpu, const float* pos, const float* m, float f, int exclude, int mode, std::vector<uint32_t>& pixels) {
        glUniform3fv(glGetUniformLocation(program, "camPos"), 1, pos);
        glUniformMatrix3fv(glGetUniformLocation(program, "camRot"), 1, GL_TRUE, m);
        glUniform1f(glGetUniformLocation(program, "camF"), f);
        glUniform1i(glGetUniformLocation(program, "exclude"), exclude);
        glUniform1i(glGetUniformLocation(program, "mode"), mode);
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glDrawArrays(GL_TRIANGLES, 0, gpu.vertexCount);
        pixels.resize(size_t(kBackdropW) * kBackdropH);
        glReadPixels(0, 0, kBackdropW, kBackdropH, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    }
};

double seconds(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
}

std::string upper(std::string s) {
    for (char& c : s) {
        c = char(std::toupper(static_cast<unsigned char>(c)));
    }
    return s;
}

float triangleArea(const float* v) {
    const float* a = v;
    const float* b = v + kSceneVertexFloats;
    const float* c = v + 2 * kSceneVertexFloats;
    float u[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
    float w[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
    float x = u[1] * w[2] - u[2] * w[1], y = u[2] * w[0] - u[0] * w[2], z = u[0] * w[1] - u[1] * w[0];
    return 0.5f * std::sqrt(x * x + y * y + z * z);
}

void writePanel(const fs::path& path, const std::vector<uint32_t>& backdrop, const std::vector<uint32_t>& predicted) {
    int w = kBackdropW, h = kBackdropH;
    std::vector<uint8_t> out(size_t(w) * 3 * size_t(h) * 3);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            size_t i = size_t(y) * size_t(w) + size_t(x);
            uint32_t b = backdrop[i], p = predicted[i];
            uint8_t a = uint8_t(p >> 24);
            uint8_t* row = out.data() + size_t(y) * size_t(w) * 9;
            for (int k = 0; k < 3; k++) {
                int bc = int((b >> (8 * k)) & 0xff), pc = int((p >> (8 * k)) & 0xff);
                row[size_t(x) * 3 + size_t(k)] = uint8_t(bc);
                uint8_t shown = a == 255 ? uint8_t(pc) : a ? uint8_t(k == 1 ? 0 : 200) : uint8_t(k == 2 ? 60 : 0);
                row[size_t(w + x) * 3 + size_t(k)] = shown;
                row[size_t(2 * w + x) * 3 + size_t(k)] = a == 255 ? uint8_t(std::min(255, 4 * std::abs(bc - pc))) : 0;
            }
        }
    }
    stbi_write_png(path.string().c_str(), 3 * w, h, 3, out.data(), 3 * w * 3);
}

void evaluate(const Scene& scene, const ProjectionGpu& gpu, bool useAtlas, GLuint atlasTex, int pageSize, const fs::path& imageDir, int images, const char* label) {
    ViewRenderer view(gpu, atlasTex, pageSize);
    bool novel = std::strcmp(label, "novel") == 0;
    std::vector<CameraScore> scores;
    std::vector<std::vector<uint32_t>> predictions(scene.cameras.size());
    std::vector<uint32_t> backdrop;
    for (size_t i = 0; i < scene.cameras.size(); i++) {
        const SceneCamera& cam = scene.cameras[i];
        std::vector<uint32_t>& pixels = predictions[i];
        view.render(gpu, cam.pos, cam.m, cam.f, novel ? int(i) : -1, useAtlas ? 1 : 0, pixels);
        scene_backdrop(cam.name, backdrop);
        size_t geometry = 0, covered = 0;
        double err = 0;
        for (size_t p = 0; p < pixels.size(); p++) {
            uint8_t a = uint8_t(pixels[p] >> 24);
            if (!a) {
                continue;
            }
            geometry++;
            if (a != 255) {
                continue;
            }
            covered++;
            for (int k = 0; k < 3; k++) {
                err += std::abs(int((pixels[p] >> (8 * k)) & 0xff) - int((backdrop[p] >> (8 * k)) & 0xff));
            }
        }
        scores.push_back({int(i), geometry ? double(covered) / double(geometry) : 1.0, covered ? err / (3.0 * double(covered)) : 0.0, geometry});
    }
    size_t geometry = 0;
    double covered = 0, err = 0;
    for (const CameraScore& s : scores) {
        geometry += s.geometry;
        covered += s.coverage * double(s.geometry);
        err += s.error * s.coverage * double(s.geometry);
    }
    std::printf("  %s: coverage %.1f%%, mean error %.1f\n", label, 100.0 * covered / double(std::max<size_t>(geometry, 1)), err / std::max(covered, 1.0));
    if (images > 0) {
        std::vector<CameraScore> sorted = scores;
        std::sort(sorted.begin(), sorted.end(), [](const CameraScore& a, const CameraScore& b) { return a.error * a.coverage + 40.0 * (1.0 - a.coverage) > b.error * b.coverage + 40.0 * (1.0 - b.coverage); });
        fs::create_directories(imageDir);
        for (int k = 0; k < images && k < int(sorted.size()); k++) {
            const CameraScore& s = sorted[size_t(k)];
            const SceneCamera& cam = scene.cameras[size_t(s.camera)];
            std::printf("    worst %d: %s coverage %.1f%% error %.1f\n", k, cam.name.c_str(), 100.0 * s.coverage, s.error);
            scene_backdrop(cam.name, backdrop);
            writePanel(imageDir / (std::string(label) + "_" + std::to_string(k) + "_" + cam.name + ".png"), backdrop, predictions[size_t(s.camera)]);
        }
        std::vector<CameraScore> overlapping;
        for (const CameraScore& s : scores) {
            if (s.coverage > 0.5) {
                overlapping.push_back(s);
            }
        }
        std::sort(overlapping.begin(), overlapping.end(), [](const CameraScore& a, const CameraScore& b) { return a.error > b.error; });
        for (int k = 0; k < images && k < int(overlapping.size()); k++) {
            const CameraScore& s = overlapping[size_t(k)];
            const SceneCamera& cam = scene.cameras[size_t(s.camera)];
            std::printf("    highest error %d: %s coverage %.1f%% error %.1f\n", k, cam.name.c_str(), 100.0 * s.coverage, s.error);
            scene_backdrop(cam.name, backdrop);
            writePanel(imageDir / (std::string(label) + "_error_" + std::to_string(k) + "_" + cam.name + ".png"), backdrop, predictions[size_t(s.camera)]);
        }
    }
}

void evaluateFirstPerson(const Scene& scene, const ProjectionGpu& gpu, const std::vector<uint8_t>& visible, GLuint atlasTex, int pageSize, const fs::path& imageDir, int images) {
    constexpr float kEyeHeight = 5.6f;
    constexpr int kViews = 48;
    std::vector<size_t> floors;
    std::vector<double> cumulative;
    double total = 0;
    for (size_t f = 0; f < visible.size(); f++) {
        const float* v = scene.verts.data() + f * 3 * kSceneVertexFloats;
        if (visible[f] && v[4] > 0.7f) {
            total += triangleArea(v);
            floors.push_back(f);
            cumulative.push_back(total);
        }
    }
    if (floors.empty()) {
        return;
    }
    ViewRenderer view(gpu, atlasTex, pageSize);
    uint64_t state = 0x9e3779b97f4a7c15ULL;
    auto random = [&] {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return double(state >> 11) / double(1ULL << 53);
    };
    const float f = 320.0f / std::tan(40.0f * 3.14159265f / 180.0f);
    size_t sampled = 0, filled = 0, unseen = 0, geometry = 0;
    int views = 0, attempts = 0;
    std::vector<std::vector<uint32_t>> shots;
    std::vector<uint32_t> pixels;
    while (views < kViews && attempts < kViews * 20) {
        attempts++;
        size_t face = floors[size_t(std::lower_bound(cumulative.begin(), cumulative.end(), random() * total) - cumulative.begin())];
        const float* v = scene.verts.data() + face * 3 * kSceneVertexFloats;
        double a = random(), b = random();
        if (a + b > 1) {
            a = 1 - a;
            b = 1 - b;
        }
        float eye[3];
        for (int k = 0; k < 3; k++) {
            eye[k] = float(v[k] + a * (v[kSceneVertexFloats + k] - v[k]) + b * (v[2 * kSceneVertexFloats + k] - v[k]));
        }
        eye[1] += kEyeHeight;
        double yaw = random() * 2 * 3.14159265358979, pitch = -10.0 * 3.14159265358979 / 180.0;
        float fwd[3] = {float(std::cos(pitch) * std::sin(yaw)), float(std::sin(pitch)), float(std::cos(pitch) * std::cos(yaw))};
        float right[3] = {fwd[2], 0.0f, -fwd[0]};
        float rl = std::sqrt(right[0] * right[0] + right[2] * right[2]);
        right[0] /= rl;
        right[2] /= rl;
        float up[3] = {fwd[1] * right[2] - fwd[2] * right[1], fwd[2] * right[0] - fwd[0] * right[2], fwd[0] * right[1] - fwd[1] * right[0]};
        float m[9];
        for (int r = 0; r < 3; r++) {
            m[r * 3 + 0] = right[r];
            m[r * 3 + 1] = up[r];
            m[r * 3 + 2] = fwd[r];
        }
        view.render(gpu, eye, m, f, -1, 2, pixels);
        size_t g = 0, back = 0, s = 0, fl = 0, u = 0;
        for (uint32_t p : pixels) {
            uint8_t al = uint8_t(p >> 24);
            g += al != 0;
            back += al == 32;
            u += al == 64;
            fl += al == 128;
            s += al == 255;
        }
        if (g < pixels.size() / 2 || back * 5 > g) {
            continue;
        }
        views++;
        sampled += s;
        filled += fl;
        unseen += u;
        geometry += g - back;
        if (int(shots.size()) < images) {
            shots.push_back(pixels);
        }
    }
    double gd = double(std::max<size_t>(geometry, 1));
    std::printf("  first person (%d views): sampled %.1f%%, filled %.1f%%, unseen %.1f%%\n", views, 100.0 * double(sampled) / gd, 100.0 * double(filled) / gd, 100.0 * double(unseen) / gd);
    fs::create_directories(imageDir);
    for (size_t k = 0; k < shots.size(); k++) {
        std::vector<uint8_t> out(size_t(kBackdropW) * kBackdropH * 6);
        for (size_t i = 0; i < shots[k].size(); i++) {
            uint32_t p = shots[k][i];
            uint8_t al = uint8_t(p >> 24);
            size_t y = i / size_t(kBackdropW), x = i % size_t(kBackdropW);
            uint8_t* plain = out.data() + (y * size_t(kBackdropW) * 2 + x) * 3;
            uint8_t* marked = plain + size_t(kBackdropW) * 3;
            for (int c = 0; c < 3; c++) {
                uint8_t col = uint8_t(p >> (8 * c));
                plain[c] = al == 64 ? 40 : al == 32 ? 0 : col;
                marked[c] = al == 64 ? uint8_t(c == 1 ? 0 : 255) : al == 128 ? uint8_t(c == 2 ? col / 3 : 128 + col / 2) : al == 32 ? uint8_t(c == 2 ? 120 : 0) : col;
            }
        }
        stbi_write_png((imageDir / ("fp_" + std::to_string(k) + ".png")).string().c_str(), 2 * kBackdropW, kBackdropH, 3, out.data(), 2 * kBackdropW * 3);
    }
}

void exportScene(const Scene& scene, const BakeAtlas& atlas, const fs::path& dir, const fs::path& bakedDir) {
    fs::create_directories(dir / "backdrops");
    std::vector<uint32_t> rgba;
    std::vector<uint8_t> rgb(size_t(kBackdropW) * kBackdropH * 3);
    std::ofstream json(dir / "scene.json");
    json << "{\n  \"name\": \"" << scene_name(scene.setKey) << "\",\n  \"pageSize\": " << atlas.pageSize << ",\n  \"pages\": [";
    for (int p = 0; p < atlas.pages; p++) {
        json << (p ? ", " : "") << '"' << fs::absolute(bakedDir / ("page" + std::to_string(p) + ".png")).string() << '"';
    }
    json << "],\n  \"cameras\": [\n";
    for (size_t i = 0; i < scene.cameras.size(); i++) {
        const SceneCamera& cam = scene.cameras[i];
        scene_backdrop(cam.name, rgba);
        for (size_t p = 0; p < rgba.size(); p++) {
            rgb[p * 3 + 0] = uint8_t(rgba[p]);
            rgb[p * 3 + 1] = uint8_t(rgba[p] >> 8);
            rgb[p * 3 + 2] = uint8_t(rgba[p] >> 16);
        }
        stbi_write_png((dir / "backdrops" / (cam.name + ".png")).string().c_str(), kBackdropW, kBackdropH, 3, rgb.data(), kBackdropW * 3);
        json << "    {\"name\": \"" << cam.name << "\", \"pos\": [" << cam.pos[0] << ", " << cam.pos[1] << ", " << cam.pos[2] << "], \"m\": [";
        for (int k = 0; k < 9; k++) {
            json << (k ? ", " : "") << cam.m[k];
        }
        json << "], \"f\": " << cam.f << "}" << (i + 1 < scene.cameras.size() ? "," : "") << "\n";
    }
    json << "  ]\n}\n";
    size_t vertices = scene.verts.size() / kSceneVertexFloats;
    std::ofstream mesh(dir / "mesh.bin", std::ios::binary);
    uint32_t header[2] = {uint32_t(vertices), atlas.uvPage.empty() ? 0u : 1u};
    mesh.write(reinterpret_cast<const char*>(header), sizeof header);
    mesh.write(reinterpret_cast<const char*>(scene.verts.data()), std::streamsize(scene.verts.size() * 4));
    if (!atlas.uvPage.empty()) {
        mesh.write(reinterpret_cast<const char*>(atlas.uvPage.data()), std::streamsize(atlas.uvPage.size() * 4));
    }
}

void writeLevelList(const fs::path& outDir) {
    std::vector<std::string> names;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(outDir, ec)) {
        if (fs::exists(e.path() / "scene.json")) {
            names.push_back(e.path().filename().string());
        }
    }
    std::sort(names.begin(), names.end());
    std::ofstream out(outDir / "levels.json");
    out << "[";
    for (size_t i = 0; i < names.size(); i++) {
        out << (i ? ", " : "") << '"' << names[i] << '"';
    }
    out << "]\n";
}

GLuint uploadPages(const BakeAtlas& atlas, const std::vector<std::vector<uint32_t>>& pages) {
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D_ARRAY, tex);
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, atlas.pageSize, atlas.pageSize, atlas.pages, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    for (int p = 0; p < atlas.pages; p++) {
        glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, p, atlas.pageSize, atlas.pageSize, 1, GL_RGBA, GL_UNSIGNED_BYTE, pages[size_t(p)].data());
    }
    glGenerateMipmap(GL_TEXTURE_2D_ARRAY);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    return tex;
}

bool processLevel(const Options& opt, const std::string& setKey) {
    auto start = std::chrono::steady_clock::now();
    Scene scene;
    if (!scene_load(setKey, scene)) {
        return false;
    }
    std::string name = scene_name(setKey);
    ProjectionGpu gpu;
    if (!projection_create(scene, gpu)) {
        return false;
    }
    std::vector<float> density = projection_face_density(gpu, scene);
    std::vector<uint8_t> visible(density.size());
    for (size_t f = 0; f < density.size(); f++) {
        visible[f] = density[f] > 0.0f;
    }
    double area = 0, visibleArea = 0;
    size_t visibleCount = 0;
    for (size_t f = 0; f < visible.size(); f++) {
        float a = triangleArea(scene.verts.data() + f * 3 * kSceneVertexFloats);
        area += a;
        if (visible[f]) {
            visibleArea += a;
            visibleCount++;
        }
    }
    std::printf("%s: %zu triangles, %zu cameras, %zu faces seen by a camera (%.1f%% of area)\n", name.c_str(), visible.size(), scene.cameras.size(), visibleCount, 100.0 * visibleArea / std::max(area, 1e-9));
    fs::path imageDir = opt.outDir / name / "eval";
    if (opt.eval) {
        evaluate(scene, gpu, false, 0, 0, imageDir, opt.images, "self");
        evaluate(scene, gpu, false, 0, 0, imageDir, opt.images, "novel");
    }
    BakeAtlas atlas;
    fs::path bakedDir = opt.gameDir / "baked" / name;
    if (opt.bake) {
        BakeInput input;
        input.verts = scene.verts;
        input.faceDensity = density;
        auto step = std::chrono::steady_clock::now();
        if (!bake_unwrap(input, atlas)) {
            return false;
        }
        std::printf("  unwrap %.1f s\n", seconds(step));
        step = std::chrono::steady_clock::now();
        projection_attach_uv(gpu, atlas);
        std::vector<std::vector<uint32_t>> pages;
        projection_bake(gpu, atlas, pages);
        std::printf("  projection %.1f s\n", seconds(step));
        size_t inside = 0, covered = 0;
        for (std::vector<uint32_t>& page : pages) {
            for (uint32_t& p : page) {
                uint8_t a = uint8_t(p >> 24);
                inside += a != 0;
                covered += a == 255;
            }
        }
        std::printf("  atlas: %d pages, %.1f%% of chart texels sampled\n", atlas.pages, 100.0 * double(covered) / double(std::max<size_t>(inside, 1)));
        for (std::vector<uint32_t>& page : pages) {
            bake_fill(page, atlas.pageSize);
        }
        if (opt.eval) {
            GLuint tex = uploadPages(atlas, pages);
            evaluate(scene, gpu, true, tex, atlas.pageSize, imageDir, opt.images, "atlas");
            evaluateFirstPerson(scene, gpu, visible, tex, atlas.pageSize, imageDir, opt.images);
            glDeleteTextures(1, &tex);
        }
        if (!bake_save(bakedDir, bake_hash(scene.geoBytes, scene.setBytes), atlas, pages)) {
            return false;
        }
    }
    if (!opt.bake) {
        std::vector<std::vector<uint32_t>> pages;
        if (!bake_load(bakedDir, bake_hash(scene.geoBytes, scene.setBytes), scene.verts.size() / kSceneVertexFloats, atlas, pages)) {
            std::printf("  no current bake in %s\n", bakedDir.string().c_str());
            atlas = BakeAtlas{};
        }
    }
    if (opt.exportScene) {
        exportScene(scene, atlas, opt.outDir / name, bakedDir);
    }
    projection_destroy(gpu);
    std::printf("  done in %.1f s\n", seconds(start));
    return true;
}

}

int main(int argc, char** argv) {
    Options opt;
    std::vector<std::string> args(argv + 1, argv + argc);
    for (size_t i = 0; i < args.size(); i++) {
        if (args[i] == "--out" && i + 1 < args.size()) {
            opt.outDir = args[++i];
        } else if (args[i] == "--eval") {
            opt.eval = true;
        } else if (args[i] == "--no-bake") {
            opt.bake = false;
        } else if (args[i] == "--export") {
            opt.exportScene = true;
        } else if (args[i] == "--images" && i + 1 < args.size()) {
            opt.images = std::stoi(args[++i]);
        } else if (opt.gameDir.empty()) {
            opt.gameDir = args[i];
        } else {
            opt.levels.push_back(upper(args[i]));
        }
    }
    if (opt.gameDir.empty() || opt.levels.empty()) {
        std::fprintf(stderr, "usage: nocbake <gameDir> [--out DIR] [--eval] [--images N] [--no-bake] [--export] LEVEL... | all\n");
        return 2;
    }
#ifndef _WIN32
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "offscreen");
#endif
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_Window* window = SDL_CreateWindow("nocbake", 16, 16, SDL_WINDOW_HIDDEN | SDL_WINDOW_OPENGL);
    SDL_GLContext context = window ? SDL_GL_CreateContext(window) : nullptr;
    if (!context) {
        std::fprintf(stderr, "no OpenGL context: %s\n", SDL_GetError());
        return 1;
    }
    if (!gl_load()) {
        return 1;
    }
    scene_index(opt.gameDir);
    std::vector<std::string> sets;
    for (const std::string& set : scene_sets()) {
        std::string name = scene_name(set);
        if (std::find(opt.levels.begin(), opt.levels.end(), "ALL") != opt.levels.end() || std::find(opt.levels.begin(), opt.levels.end(), name) != opt.levels.end()) {
            sets.push_back(set);
        }
    }
    int failed = 0;
    for (const std::string& set : sets) {
        if (!processLevel(opt, set)) {
            std::fprintf(stderr, "%s failed\n", set.c_str());
            failed++;
        }
    }
    if (sets.empty()) {
        std::fprintf(stderr, "no matching level\n");
        failed = 1;
    }
    if (opt.exportScene) {
        writeLevelList(opt.outDir);
    }
    SDL_GL_DestroyContext(context);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return failed ? 1 : 0;
}
