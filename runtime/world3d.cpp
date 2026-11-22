#include "world3d.h"

#include "bake.h"
#include "guest.h"
#include "options.h"
#include "projection.h"
#include "scene.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr uint32_t kGamePtr = 0x5b8094;
constexpr uint32_t kActiveCamera = 0x01fb64d8;
constexpr uint32_t kLightCount = 0x01fb79a0;
constexpr uint32_t kLightArray = 0x01fb79a4;
constexpr int kMaxLights = 96;
constexpr int kMaxDynamicLights = 4;
constexpr int kShadowSize = 512;
constexpr int kFilterSize = 256;
constexpr int kLightFloats = 20;

struct LightInfo {
    float pos[3];
    float m[9];
    float fx;
    float fy;
    float w;
    float h;
    float circle;
    float filterScaleX;
    float filterScaleY;
    float filterMask;
    float range;
    float rangeScale;
    std::vector<uint8_t> filter;
};

enum class BakeStage { Idle, Loading, Unwrapping, Saving, Ready, Failed };

struct BakeState {
    BakeStage stage = BakeStage::Idle;
    std::future<bool> work;
    BakeInput input;
    BakeAtlas atlas;
    std::vector<std::vector<uint32_t>> pages;
    uint64_t hash = 0;
    fs::path dir;
    std::chrono::steady_clock::time_point start;
};

struct Level {
    Scene scene;
    ProjectionGpu gpu;
    std::vector<LightInfo> lights;
    float ambient = 0;
    GLuint filters = 0;
    GLuint shadowMaps = 0;
    GLuint lightUbo = 0;
    GLuint shadowFbo = 0;
    GLuint shadowDepth = 0;
    GLuint dynamicFilters = 0;
    GLuint dynamicShadows = 0;
    GLuint dynamicUbo = 0;
    GLuint atlas = 0;
    BakeState bake;
};

fs::path g_gameDir;
bool g_indexed = false;
Level g_level;
GLuint g_worldProgram = 0;
std::vector<LightInfo> g_dynamicLights;

const char* kWorldVS = R"(#version 330 core
layout(location = 0) in vec3 pos;
layout(location = 1) in vec3 normal;
layout(location = 3) in vec3 uvPage;
uniform vec3 viewPos;
uniform mat3 viewRot;
uniform float viewF;
out vec3 worldPos;
out vec3 worldNormal;
out float viewDepth;
out vec2 atlasUv;
flat out float atlasPage;
void main() {
    worldPos = pos;
    worldNormal = normal;
    atlasUv = uvPage.xy;
    atlasPage = uvPage.z;
    vec3 d = pos - viewPos;
    vec3 c = vec3(dot(viewRot[0], d), dot(viewRot[1], d), dot(viewRot[2], d));
    const float near = 0.1;
    const float far = 4000.0;
    float zc = c.z * (far + near) / (far - near) - 2.0 * far * near / (far - near);
    gl_Position = vec4((viewF * c.x + 0.5 * c.z) / 320.0, (0.5 * c.z - viewF * c.y) / 240.0, zc, c.z);
    viewDepth = c.z;
}
)";

const char* kWorldFS = R"(
struct Light {
    vec4 posFx;
    vec4 right;
    vec4 up;
    vec4 fwd;
    vec4 extra;
};
layout(std140) uniform Lights {
    Light lights[96];
};
layout(std140) uniform DynamicLights {
    Light dynamicLights[4];
};
uniform int lightCount;
uniform int dynamicCount;
uniform vec2 dynamicFalloff[4];
uniform float ambient;
uniform sampler2DArray filters;
uniform sampler2DArray shadowMaps;
uniform sampler2DArray dynamicFilters;
uniform sampler2DArray dynamicShadows;
uniform vec3 eyePos;
uniform int debugColors;
uniform float zfar;
uniform float farScale;
uniform int linearDepth;
uniform int baked;
uniform float pageSize;
uniform sampler2DArray atlas;
in vec3 worldPos;
in vec3 worldNormal;
in float viewDepth;
in vec2 atlasUv;
flat in float atlasPage;
out vec4 color;
float lightTerm(Light l, int layer, sampler2DArray filterLayers, sampler2DArray shadowLayers, vec3 p, vec3 n, out float z) {
    vec3 d = p - l.posFx.xyz;
    z = dot(l.fwd.xyz, d);
    if (z < 1.0 / 256.0 || dot(n, l.fwd.xyz) > 0.0) {
        return 0.0;
    }
    float w = l.right.w;
    float h = l.up.w;
    float px = 0.5 * w + l.posFx.w * dot(l.right.xyz, d) / z;
    float py = 0.5 * h - l.extra.x * dot(l.up.xyz, d) / z;
    if (l.fwd.w != 0.0) {
        float r = 0.5 * w;
        if ((px - r) * (px - r) + (py - r) * (py - r) > r * r) {
            return 0.0;
        }
    } else if (px < 0.0 || px >= w || py < 0.0 || py >= h) {
        return 0.0;
    }
    vec2 uv = vec2(px / w, py / h);
    float lit = 0.0;
    for (int k = 0; k < 9; k++) {
        vec2 offset = vec2(float(k % 3 - 1) / w, float(k / 3 - 1) / h);
        lit += z - 0.5 <= texture(shadowLayers, vec3(uv + offset, float(layer))).r ? 1.0 : 0.0;
    }
    ivec2 texel = ivec2(floor(vec2(px * l.extra.y, py * l.extra.z))) & ivec2(int(l.extra.w));
    return texelFetch(filterLayers, ivec3(texel, layer), 0).r * 255.0 * lit / 9.0;
}
float lightLevel(vec3 p, vec3 n) {
    float level = ambient;
    float z;
    for (int j = 0; j < lightCount; j++) {
        level += lightTerm(lights[j], j, filters, shadowMaps, p, n, z);
    }
    for (int j = 0; j < dynamicCount; j++) {
        float term = lightTerm(dynamicLights[j], j, dynamicFilters, dynamicShadows, p, n, z);
        level += term * dynamicFalloff[j].y * max(1.0 - z / dynamicFalloff[j].x, 0.0);
    }
    return min(level, 255.0);
}
void main() {
    float z = max(floor(viewDepth * 256.0), 1.0);
    gl_FragDepth = linearDepth != 0 ? min(z / (zfar * farScale), 1.0) : 1.0 - 1.0 / clamp(z * 256.0 / zfar, 1.0, 256.0 * farScale);
    vec3 n = normalize(worldNormal);
    if (debugColors == 2) {
        color = vec4(n * 0.5 + 0.5, 1.0);
        return;
    }
    if (dot(n, eyePos - worldPos) < 0.0) {
        color = debugColors != 0 ? vec4(0.5, 0.0, 0.0, 1.0) : vec4(vec3(0.04), 1.0);
        return;
    }
    vec3 albedo;
    if (baked != 0 && atlasPage >= 0.0) {
        vec4 t = texture(atlas, vec3(atlasUv / pageSize, atlasPage));
        albedo = debugColors != 0 && t.a < 0.75 ? vec3(0.8, 0.8, 0.0) : t.rgb;
    } else if (projectColor(worldPos, n, -1, albedo) == 0) {
        color = vec4(debugColors != 0 ? vec3(0.0, 0.0, 0.25) * (0.6 + 0.4 * abs(n.y)) : vec3(0.06) * (0.6 + 0.4 * abs(n.y)), 1.0);
        return;
    }
    float L = lightLevel(worldPos, n);
    color = vec4(min(albedo * (L * (255.0 - 0.5 * L) / 16384.0), vec3(1.0)), 1.0);
}
)";

double secondsSince(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
}

bool finished(std::future<bool>& f) {
    return f.valid() && f.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
}

void initGl() {
    std::string fs = std::string("#version 330 core\n") + kProjectionGlsl + kWorldFS;
    g_worldProgram = projection_link(kWorldVS, fs.c_str());
    glUniformBlockBinding(g_worldProgram, glGetUniformBlockIndex(g_worldProgram, "Lights"), 0);
    glUniformBlockBinding(g_worldProgram, glGetUniformBlockIndex(g_worldProgram, "DynamicLights"), 1);
}

void freeLevel() {
    if (g_level.bake.work.valid()) {
        g_level.bake.work.wait();
    }
    projection_destroy(g_level.gpu);
    glDeleteTextures(1, &g_level.atlas);
    glDeleteTextures(1, &g_level.filters);
    glDeleteTextures(1, &g_level.shadowMaps);
    glDeleteBuffers(1, &g_level.lightUbo);
    glDeleteFramebuffers(1, &g_level.shadowFbo);
    glDeleteRenderbuffers(1, &g_level.shadowDepth);
    glDeleteTextures(1, &g_level.dynamicFilters);
    glDeleteTextures(1, &g_level.dynamicShadows);
    glDeleteBuffers(1, &g_level.dynamicUbo);
    g_level = Level{};
}

bool readLight(uint32_t p, LightInfo& l) {
    float fov = rd<float>(g_mem, p + 0x138);
    uint32_t w = rd32(g_mem, p + 0x1cc0), h = rd32(g_mem, p + 0x1cc4);
    uint32_t filter = rd32(g_mem, p + 0x1c54);
    uint32_t sx = rd32(g_mem, p + 0x1cc8), sy = rd32(g_mem, p + 0x1ccc);
    uint32_t rowShift = rd32(g_mem, p + 0x1c60), mask = rd32(g_mem, p + 0x1c68);
    bool sampled = sx < 32 && sy < 32 && rowShift < 16 && mask < uint32_t(kFilterSize) && (1u << rowShift) > mask;
    if (!(fov > 0) || !filter || !sampled || w == 0 || h == 0) {
        return false;
    }
    l = LightInfo{};
    for (int k = 0; k < 3; k++) {
        l.pos[k] = rd<float>(g_mem, p + 0x104 + 4 * k);
    }
    for (int k = 0; k < 9; k++) {
        l.m[k] = rd<float>(g_mem, p + 0x110 + 4 * k);
    }
    int32_t aspect = int32_t(rd32(g_mem, p + 0x2fa4));
    l.fx = (float(w) / 2.0f - 1.0f) * fov / 18.0f;
    l.fy = (float(h) / 2.0f - 1.0f) * (float(w) / float(h)) * (aspect ? float(aspect) / 65536.0f : 1.0f) * fov / 18.0f;
    l.w = float(w);
    l.h = float(h);
    l.circle = rd32(g_mem, p + 0x1cb8) ? 1.0f : 0.0f;
    l.filterScaleX = std::ldexp(1.0f, 16 - int(sx));
    l.filterScaleY = std::ldexp(1.0f, 16 - int(sy));
    l.filterMask = float(mask);
    l.filter.assign(size_t(kFilterSize) * kFilterSize, 0);
    for (uint32_t y = 0; y <= mask; y++) {
        std::memcpy(l.filter.data() + size_t(y) * kFilterSize, g_mem + filter + (y << rowShift), mask + 1);
    }
    return true;
}

std::vector<LightInfo> readLights() {
    std::vector<LightInfo> out;
    uint32_t n = std::min<uint32_t>(rd32(g_mem, kLightCount), kMaxLights);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t p = rd32(g_mem, kLightArray + 4 * i);
        if (!p) {
            continue;
        }
        LightInfo l;
        if (!readLight(p, l)) {
            std::fprintf(stderr, "[world3d] skipping light %s\n", reinterpret_cast<const char*>(g_mem + p));
            continue;
        }
        out.push_back(std::move(l));
    }
    return out;
}

void packLight(const LightInfo& l, float* u) {
    for (int r = 0; r < 3; r++) {
        u[0 + r] = l.pos[r];
        u[4 + r] = l.m[r * 3 + 0];
        u[8 + r] = l.m[r * 3 + 1];
        u[12 + r] = l.m[r * 3 + 2];
    }
    u[3] = l.fx;
    u[7] = l.w;
    u[11] = l.h;
    u[15] = l.circle;
    u[16] = l.fy;
    u[17] = l.filterScaleX;
    u[18] = l.filterScaleY;
    u[19] = l.filterMask;
}

void renderShadowLayers(const std::vector<LightInfo>& lights, GLuint filters, GLuint shadowMaps) {
    glBindFramebuffer(GL_FRAMEBUFFER, g_level.shadowFbo);
    glViewport(0, 0, kShadowSize, kShadowSize);
    GLuint program = projection_depth_program();
    glUseProgram(program);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glBindVertexArray(g_level.gpu.vao);
    const float far[4] = {1e9f, 1e9f, 1e9f, 1e9f};
    for (size_t j = 0; j < lights.size(); j++) {
        const LightInfo& l = lights[j];
        glBindTexture(GL_TEXTURE_2D_ARRAY, filters);
        glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, GLint(j), kFilterSize, kFilterSize, 1, GL_RED, GL_UNSIGNED_BYTE, l.filter.data());
        glUniform3fv(glGetUniformLocation(program, "camPos"), 1, l.pos);
        glUniformMatrix3fv(glGetUniformLocation(program, "camRot"), 1, GL_TRUE, l.m);
        glUniform2f(glGetUniformLocation(program, "camF"), l.fx, l.fy);
        glUniform2f(glGetUniformLocation(program, "halfSize"), l.w / 2.0f, l.h / 2.0f);
        glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, shadowMaps, 0, GLint(j));
        glClearBufferfv(GL_COLOR, 0, far);
        glClear(GL_DEPTH_BUFFER_BIT);
        glDrawArrays(GL_TRIANGLES, 0, g_level.gpu.vertexCount);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

GLuint makeLayers(GLenum internal, int size, int layers) {
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D_ARRAY, tex);
    bool isFloat = internal == GL_R32F;
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GLint(internal), size, size, std::max(layers, 1), 0, GL_RED, isFloat ? GL_FLOAT : GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return tex;
}

void buildLights() {
    GLsizei nl = GLsizei(g_level.lights.size());
    g_level.filters = makeLayers(GL_R8, kFilterSize, nl);
    g_level.shadowMaps = makeLayers(GL_R32F, kShadowSize, nl);
    g_level.dynamicFilters = makeLayers(GL_R8, kFilterSize, kMaxDynamicLights);
    g_level.dynamicShadows = makeLayers(GL_R32F, kShadowSize, kMaxDynamicLights);
    glGenRenderbuffers(1, &g_level.shadowDepth);
    glBindRenderbuffer(GL_RENDERBUFFER, g_level.shadowDepth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT32F, kShadowSize, kShadowSize);
    glGenFramebuffers(1, &g_level.shadowFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, g_level.shadowFbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, g_level.shadowDepth);
    renderShadowLayers(g_level.lights, g_level.filters, g_level.shadowMaps);
    std::vector<float> lightData(size_t(kMaxLights) * kLightFloats, 0.0f);
    for (size_t j = 0; j < g_level.lights.size(); j++) {
        packLight(g_level.lights[j], lightData.data() + j * kLightFloats);
    }
    glGenBuffers(1, &g_level.lightUbo);
    glBindBuffer(GL_UNIFORM_BUFFER, g_level.lightUbo);
    glBufferData(GL_UNIFORM_BUFFER, GLsizeiptr(lightData.size() * 4), lightData.data(), GL_STATIC_DRAW);
    glGenBuffers(1, &g_level.dynamicUbo);
    glBindBuffer(GL_UNIFORM_BUFFER, g_level.dynamicUbo);
    glBufferData(GL_UNIFORM_BUFFER, GLsizeiptr(kMaxDynamicLights * kLightFloats * 4), nullptr, GL_DYNAMIC_DRAW);
}

void updateDynamicLights() {
    if (g_dynamicLights.empty()) {
        return;
    }
    renderShadowLayers(g_dynamicLights, g_level.dynamicFilters, g_level.dynamicShadows);
    std::vector<float> data(size_t(kMaxDynamicLights) * kLightFloats, 0.0f);
    for (size_t j = 0; j < g_dynamicLights.size(); j++) {
        packLight(g_dynamicLights[j], data.data() + j * kLightFloats);
    }
    glBindBuffer(GL_UNIFORM_BUFFER, g_level.dynamicUbo);
    glBufferSubData(GL_UNIFORM_BUFFER, 0, GLsizeiptr(data.size() * 4), data.data());
}

bool loadLevel(const std::string& setKey) {
    freeLevel();
    if (!scene_load(setKey, g_level.scene) || !projection_create(g_level.scene, g_level.gpu)) {
        return false;
    }
    g_level.lights = readLights();
    g_level.ambient = float((int32_t(rd32(g_mem, kActiveCamera + 0x11e8)) / 256) / 4);
    std::fprintf(stderr, "[world3d] %zu of %u lights, ambient %.0f\n", g_level.lights.size(), rd32(g_mem, kLightCount), g_level.ambient);
    buildLights();
    BakeState& b = g_level.bake;
    b.hash = bake_hash(g_level.scene.geoBytes, g_level.scene.setBytes);
    b.dir = g_gameDir / "baked" / scene_name(setKey);
    return true;
}

void uploadAtlas(BakeState& b) {
    int size = b.atlas.pageSize;
    glGenTextures(1, &g_level.atlas);
    glBindTexture(GL_TEXTURE_2D_ARRAY, g_level.atlas);
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, size, size, b.atlas.pages, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    for (int p = 0; p < b.atlas.pages; p++) {
        glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, p, size, size, 1, GL_RGBA, GL_UNSIGNED_BYTE, b.pages[size_t(p)].data());
    }
    glGenerateMipmap(GL_TEXTURE_2D_ARRAY);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameterf(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_ANISOTROPY_EXT, 8.0f);
    b.pages = {};
}

void pumpBake() {
    BakeState& b = g_level.bake;
    switch (b.stage) {
    case BakeStage::Idle:
        b.start = std::chrono::steady_clock::now();
        b.stage = BakeStage::Loading;
        b.work = std::async(std::launch::async, [&b, n = g_level.scene.verts.size() / kSceneVertexFloats] { return bake_load(b.dir, b.hash, n, b.atlas, b.pages); });
        break;
    case BakeStage::Loading:
        if (!finished(b.work)) {
            break;
        }
        if (b.work.get()) {
            projection_attach_uv(g_level.gpu, b.atlas);
            uploadAtlas(b);
            b.stage = BakeStage::Ready;
            std::fprintf(stderr, "[bake] loaded %s in %.1f s\n", b.dir.string().c_str(), secondsSince(b.start));
            break;
        }
        std::fprintf(stderr, "[bake] baking %s\n", b.dir.string().c_str());
        b.input.verts = g_level.scene.verts;
        b.input.faceDensity = projection_face_density(g_level.gpu, g_level.scene);
        b.stage = BakeStage::Unwrapping;
        b.work = std::async(std::launch::async, [&b] { return bake_unwrap(b.input, b.atlas); });
        break;
    case BakeStage::Unwrapping: {
        if (!finished(b.work)) {
            break;
        }
        if (!b.work.get()) {
            b.stage = BakeStage::Failed;
            break;
        }
        b.input = BakeInput{};
        double unwrapped = secondsSince(b.start);
        projection_attach_uv(g_level.gpu, b.atlas);
        projection_bake(g_level.gpu, b.atlas, b.pages);
        std::fprintf(stderr, "[bake] unwrap %.1f s, GPU bake %.1f s\n", unwrapped, secondsSince(b.start) - unwrapped);
        b.stage = BakeStage::Saving;
        b.work = std::async(std::launch::async, [&b] {
            for (std::vector<uint32_t>& page : b.pages) {
                bake_fill(page, b.atlas.pageSize);
            }
            return bake_save(b.dir, b.hash, b.atlas, b.pages);
        });
        break;
    }
    case BakeStage::Saving:
        if (!finished(b.work)) {
            break;
        }
        if (!b.work.get()) {
            std::fprintf(stderr, "[bake] could not save %s\n", b.dir.string().c_str());
        }
        uploadAtlas(b);
        b.stage = BakeStage::Ready;
        std::fprintf(stderr, "[bake] %s ready after %.1f s\n", b.dir.string().c_str(), secondsSince(b.start));
        break;
    default:
        break;
    }
}

}

void world3d_init(const fs::path& gameDir) {
    g_gameDir = gameDir;
}

void world3d_capture_dynamic_lights(const std::vector<uint32_t>& lights) {
    g_dynamicLights.clear();
    for (uint32_t p : lights) {
        LightInfo l;
        float range = rd<float>(g_mem, p + 0x100);
        if (g_dynamicLights.size() >= kMaxDynamicLights || !(range > 0) || !readLight(p, l)) {
            continue;
        }
        long double maxDepth = (long double)range * 256.0L;
        int shift = int(std::log10(maxDepth) * 3.32192809489L) - 1;
        l.range = range;
        l.rangeScale = float(std::trunc(maxDepth) / std::ldexp(1.0L, shift));
        g_dynamicLights.push_back(std::move(l));
    }
}

bool world3d_active() {
    if (!g_options.texturedNightVision) {
        return false;
    }
    uint32_t game = rd32(g_mem, kGamePtr);
    return game && rd32(g_mem, game + 0x240) != 0;
}

bool world3d_render(uint32_t fbo, int width, int height, const World3dView& headView) {
    if (!g_worldProgram) {
        initGl();
    }
    if (!g_indexed) {
        scene_index(g_gameDir);
        g_indexed = true;
    }
    std::string camName(reinterpret_cast<const char*>(g_mem + kActiveCamera), strnlen(reinterpret_cast<const char*>(g_mem + kActiveCamera), 64));
    for (char& ch : camName) {
        ch = char(std::toupper(static_cast<unsigned char>(ch)));
    }
    std::string setKey = scene_set_for_camera(camName);
    if (setKey.empty()) {
        return false;
    }
    if (setKey != g_level.scene.setKey && !loadLevel(setKey)) {
        return false;
    }
    if (!g_level.gpu.vertexCount) {
        return false;
    }
    pumpBake();
    updateDynamicLights();
    World3dView view = headView;
    if (std::getenv("NOC_W3D_CAMERA_VIEW")) {
        for (const SceneCamera& cam : g_level.scene.cameras) {
            if (cam.name == camName) {
                std::copy_n(cam.pos, 3, view.pos);
                std::copy_n(cam.m, 9, view.m);
                view.f = cam.f;
            }
        }
    }

    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, width, height);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);
    glClearColor(0, 0, 0, 1);
    glClearDepth(1.0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glUseProgram(g_worldProgram);
    glUniform3fv(glGetUniformLocation(g_worldProgram, "viewPos"), 1, view.pos);
    glUniformMatrix3fv(glGetUniformLocation(g_worldProgram, "viewRot"), 1, GL_TRUE, view.m);
    glUniform1f(glGetUniformLocation(g_worldProgram, "viewF"), view.f);
    glUniform1f(glGetUniformLocation(g_worldProgram, "zfar"), view.zfar);
    glUniform1f(glGetUniformLocation(g_worldProgram, "farScale"), view.farScale);
    glUniform1i(glGetUniformLocation(g_worldProgram, "linearDepth"), view.linearDepth ? 1 : 0);
    glUniform3fv(glGetUniformLocation(g_worldProgram, "eyePos"), 1, view.pos);
    glUniform1i(glGetUniformLocation(g_worldProgram, "debugColors"), std::getenv("NOC_W3D_DEBUG_COLORS") ? std::atoi(std::getenv("NOC_W3D_DEBUG_COLORS")) : 0);
    glUniform1i(glGetUniformLocation(g_worldProgram, "lightCount"), GLint(g_level.lights.size()));
    std::vector<float> falloff;
    for (const LightInfo& l : g_dynamicLights) {
        falloff.push_back(l.range);
        falloff.push_back(l.rangeScale);
    }
    glUniform1i(glGetUniformLocation(g_worldProgram, "dynamicCount"), GLint(g_dynamicLights.size()));
    if (!falloff.empty()) {
        glUniform2fv(glGetUniformLocation(g_worldProgram, "dynamicFalloff"), GLsizei(g_dynamicLights.size()), falloff.data());
    }
    glUniform1f(glGetUniformLocation(g_worldProgram, "ambient"), g_level.ambient);
    glUniform1i(glGetUniformLocation(g_worldProgram, "baked"), g_level.bake.stage == BakeStage::Ready ? 1 : 0);
    glUniform1f(glGetUniformLocation(g_worldProgram, "pageSize"), float(g_level.bake.atlas.pageSize));
    projection_bind(g_level.gpu, g_worldProgram, 0);
    glUniform1i(glGetUniformLocation(g_worldProgram, "atlas"), 3);
    glUniform1i(glGetUniformLocation(g_worldProgram, "filters"), 4);
    glUniform1i(glGetUniformLocation(g_worldProgram, "shadowMaps"), 5);
    glUniform1i(glGetUniformLocation(g_worldProgram, "dynamicFilters"), 6);
    glUniform1i(glGetUniformLocation(g_worldProgram, "dynamicShadows"), 7);
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D_ARRAY, g_level.atlas);
    glActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_2D_ARRAY, g_level.filters);
    glActiveTexture(GL_TEXTURE5);
    glBindTexture(GL_TEXTURE_2D_ARRAY, g_level.shadowMaps);
    glActiveTexture(GL_TEXTURE6);
    glBindTexture(GL_TEXTURE_2D_ARRAY, g_level.dynamicFilters);
    glActiveTexture(GL_TEXTURE7);
    glBindTexture(GL_TEXTURE_2D_ARRAY, g_level.dynamicShadows);
    glActiveTexture(GL_TEXTURE0);
    glBindBufferBase(GL_UNIFORM_BUFFER, 0, g_level.lightUbo);
    glBindBufferBase(GL_UNIFORM_BUFFER, 1, g_level.dynamicUbo);
    glBindVertexArray(g_level.gpu.vao);
    glDrawArrays(GL_TRIANGLES, 0, g_level.gpu.vertexCount);
    return true;
}
