#include "renderer.h"

#include "display.h"
#include "gl.h"
#include "win32.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <vector>

SDL_Window* user32_window();

namespace {

struct GlVertex {
    float x, y, z, rhw;
    uint8_t diffuse[4];
    uint8_t specular[4];
    float u, v;
};

struct DrawState {
    GLuint texture = 0;
    bool textured = false;
    bool blend = false;
    bool fog = false;
    bool flat = false;
    bool depthOn = false;
    bool depthTest = false;
    bool depthWrite = false;
    bool linear = false;
    GLenum srcBlend = GL_ONE;
    GLenum dstBlend = GL_ONE_MINUS_SRC_ALPHA;
    bool operator==(const DrawState&) const = default;
};

struct Texture {
    GLuint id = 0;
    bool alphaMap = false;
};

const char* kVertexShader = R"(#version 330 core
layout(location = 0) in vec4 posZW;
layout(location = 1) in vec4 diffuse;
layout(location = 2) in vec4 specular;
layout(location = 3) in vec2 uv;
uniform vec2 gameSize;
out vec4 vDiffuse;
out vec4 vSpecular;
flat out vec4 fDiffuse;
flat out vec4 fSpecular;
out vec2 vUV;
void main() {
    float w = 1.0 / max(posZW.w, 1e-6);
    vec2 ndc = (posZW.xy + 0.5) / gameSize * 2.0 - 1.0;
    gl_Position = vec4(ndc * w, (posZW.z * 2.0 - 1.0) * w, w);
    vDiffuse = diffuse;
    vSpecular = specular;
    fDiffuse = diffuse;
    fSpecular = specular;
    vUV = uv;
}
)";

const char* kFragmentShader = R"(#version 330 core
uniform sampler2D tex;
uniform int textured;
uniform int alphaTest;
uniform int fogOn;
uniform int flatShade;
uniform vec3 fogColor;
in vec4 vDiffuse;
in vec4 vSpecular;
flat in vec4 fDiffuse;
flat in vec4 fSpecular;
in vec2 vUV;
out vec4 outColor;
void main() {
    vec4 d = flatShade != 0 ? fDiffuse : vDiffuse;
    vec3 s = flatShade != 0 ? fSpecular.rgb : vSpecular.rgb;
    vec4 c = d;
    if (textured != 0) {
        vec4 t = texture(tex, vUV);
        c = vec4(t.rgb * d.rgb, t.a * d.a);
    }
    c.rgb = min(c.rgb + s, vec3(1.0));
    if (fogOn != 0) {
        c.rgb = mix(fogColor, c.rgb, vSpecular.a);
    }
    if (alphaTest != 0 && c.a < 0.5 / 255.0) {
        discard;
    }
    outColor = c;
}
)";

const char* kOverlayVertexShader = R"(#version 330 core
out vec2 uv;
void main() {
    vec2 p = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));
    uv = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

const char* kOverlayFragmentShader = R"(#version 330 core
uniform sampler2D image;
in vec2 uv;
out vec4 color;
void main() {
    ivec2 size = textureSize(image, 0);
    ivec2 texel = clamp(ivec2(uv * vec2(size)), ivec2(0), size - ivec2(1));
    if (texelFetch(image, texel, 0).a < 0.5) {
        discard;
    }
    color = vec4(texture(image, uv).rgb, 1.0);
}
)";

struct Renderer {
    uint32_t table[35] = {};
    int gameW = 640;
    int gameH = 480;
    int bpp = 16;
    int renderW = 640;
    int renderH = 480;
    bool ready = false;
    bool inScene = false;
    bool frameLocked = false;
    uint32_t rowTable = 0;
    std::vector<uint32_t> savedRows;
    uint32_t palette = 0;
    float fogColor[3] = {0, 0, 0};
    GLenum dstBlend = GL_ONE_MINUS_SRC_ALPHA;
    float farScale = 1.0f;

    GLuint fbo = 0;
    GLuint color = 0;
    GLuint depth = 0;
    std::vector<GLuint> masterFbo;
    std::vector<GLuint> masterDepth;
    GLuint downFbo = 0;
    GLuint downColor = 0;
    GLuint overlayTex = 0;
    GLuint program = 0;
    GLuint overlayProgram = 0;
    GLuint vao = 0;
    GLuint vbo = 0;
    GLuint ibo = 0;
    GLuint emptyVao = 0;
    GLuint samplerLinear = 0;
    GLuint samplerNearest = 0;
    GLuint whiteTexture = 0;
    GLint uGameSize = -1, uTextured = -1, uAlphaTest = -1, uFogOn = -1, uFlat = -1, uFogColor = -1;

    std::unordered_map<std::string, Texture> textures;
    Texture current;

    std::vector<GlVertex> verts;
    std::vector<uint32_t> indices;
    DrawState batch;

    uint32_t mirror = 0;
    bool drawnSincePresent = false;
    std::vector<uint8_t> snapshot;
    std::vector<uint32_t> scratch;
    bool gpuDirty = true;
};

Renderer R;

void logFirst(const char* what) {
    static std::unordered_map<std::string, bool> seen;
    if (!seen[what]) {
        seen[what] = true;
        std::fprintf(stderr, "[renderer] first %s\n", what);
    }
}

uint32_t var(int i) {
    return R.table[i] ? rd32(g_mem, R.table[i]) : 0;
}

void setVar(int i, uint32_t v) {
    if (R.table[i]) {
        wr32(g_mem, R.table[i], v);
    }
}

int bytesPerPixel() {
    return R.bpp == 16 ? 2 : 4;
}

GLuint compile(GLenum type, const char* source) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &source, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetShaderInfoLog(s, sizeof log, nullptr, log);
        std::fprintf(stderr, "[renderer] shader compile failed: %s\n", log);
    }
    return s;
}

GLuint link(const char* vs, const char* fs) {
    GLuint p = glCreateProgram();
    glAttachShader(p, compile(GL_VERTEX_SHADER, vs));
    glAttachShader(p, compile(GL_FRAGMENT_SHADER, fs));
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetProgramInfoLog(p, sizeof log, nullptr, log);
        std::fprintf(stderr, "[renderer] program link failed: %s\n", log);
    }
    return p;
}

GLuint makeTexture(GLenum internal, int w, int h, GLenum format, GLenum type) {
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, internal, w, h, 0, format, type, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return t;
}

GLuint makeDepthFbo(GLuint& depthTex, GLuint colorTex) {
    depthTex = makeTexture(GL_DEPTH_COMPONENT32F, R.renderW, R.renderH, GL_DEPTH_COMPONENT, GL_FLOAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    GLuint f;
    glGenFramebuffers(1, &f);
    glBindFramebuffer(GL_FRAMEBUFFER, f);
    if (colorTex) {
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colorTex, 0);
    } else {
        glDrawBuffer(GL_NONE);
        glReadBuffer(GL_NONE);
    }
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, depthTex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        std::fprintf(stderr, "[renderer] framebuffer incomplete\n");
    }
    return f;
}

void chooseRenderSize() {
    int dw, dh;
    display_drawable_size(dw, dh);
    int h = dh;
    if (const char* env = std::getenv("NOC_RENDER_HEIGHT")) {
        h = std::max(1, std::atoi(env));
    }
    R.renderH = h;
    R.renderW = h * R.gameW / R.gameH;
}

void createTargets() {
    chooseRenderSize();
    R.color = makeTexture(GL_RGBA8, R.renderW, R.renderH, GL_RGBA, GL_UNSIGNED_BYTE);
    R.fbo = makeDepthFbo(R.depth, R.color);
    R.downColor = makeTexture(GL_RGBA8, R.gameW, R.gameH, GL_RGBA, GL_UNSIGNED_BYTE);
    glGenFramebuffers(1, &R.downFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, R.downFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, R.downColor, 0);
    R.overlayTex = makeTexture(GL_RGBA8, R.gameW, R.gameH, GL_RGBA, GL_UNSIGNED_BYTE);
    for (int i = 0; i < 8; i++) {
        GLuint d;
        R.masterFbo.push_back(makeDepthFbo(d, 0));
        R.masterDepth.push_back(d);
    }
    std::fprintf(stderr, "[renderer] game %dx%dx%d, rendering at %dx%d\n", R.gameW, R.gameH, R.bpp, R.renderW, R.renderH);
}

void createPipeline() {
    R.program = link(kVertexShader, kFragmentShader);
    R.overlayProgram = link(kOverlayVertexShader, kOverlayFragmentShader);
    R.uGameSize = glGetUniformLocation(R.program, "gameSize");
    R.uTextured = glGetUniformLocation(R.program, "textured");
    R.uAlphaTest = glGetUniformLocation(R.program, "alphaTest");
    R.uFogOn = glGetUniformLocation(R.program, "fogOn");
    R.uFlat = glGetUniformLocation(R.program, "flatShade");
    R.uFogColor = glGetUniformLocation(R.program, "fogColor");
    glGenVertexArrays(1, &R.vao);
    glBindVertexArray(R.vao);
    glGenBuffers(1, &R.vbo);
    glGenBuffers(1, &R.ibo);
    glBindBuffer(GL_ARRAY_BUFFER, R.vbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, R.ibo);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof(GlVertex), reinterpret_cast<void*>(offsetof(GlVertex, x)));
    glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(GlVertex), reinterpret_cast<void*>(offsetof(GlVertex, diffuse)));
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(GlVertex), reinterpret_cast<void*>(offsetof(GlVertex, specular)));
    glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, sizeof(GlVertex), reinterpret_cast<void*>(offsetof(GlVertex, u)));
    for (int i = 0; i < 4; i++) {
        glEnableVertexAttribArray(i);
    }
    glGenVertexArrays(1, &R.emptyVao);
    glGenSamplers(1, &R.samplerLinear);
    glSamplerParameteri(R.samplerLinear, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glSamplerParameteri(R.samplerLinear, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glGenSamplers(1, &R.samplerNearest);
    glSamplerParameteri(R.samplerNearest, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glSamplerParameteri(R.samplerNearest, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    for (GLuint s : {R.samplerLinear, R.samplerNearest}) {
        glSamplerParameteri(s, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glSamplerParameteri(s, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    uint32_t white = 0xffffffffu;
    glGenTextures(1, &R.whiteTexture);
    glBindTexture(GL_TEXTURE_2D, R.whiteTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &white);
    glProvokingVertex(GL_FIRST_VERTEX_CONVENTION);
}

void bindTarget() {
    glBindFramebuffer(GL_FRAMEBUFFER, R.fbo);
    glViewport(0, 0, R.renderW, R.renderH);
}

void flush() {
    if (R.indices.empty()) {
        return;
    }
    const DrawState& s = R.batch;
    bindTarget();
    glDisable(GL_SCISSOR_TEST);
    glUseProgram(R.program);
    glUniform2f(R.uGameSize, float(R.gameW), float(R.gameH));
    glUniform1i(R.uTextured, s.textured);
    glUniform1i(R.uAlphaTest, s.blend);
    glUniform1i(R.uFogOn, s.fog);
    glUniform1i(R.uFlat, s.flat);
    glUniform3f(R.uFogColor, R.fogColor[0], R.fogColor[1], R.fogColor[2]);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s.texture ? s.texture : R.whiteTexture);
    glBindSampler(0, s.linear ? R.samplerLinear : R.samplerNearest);
    if (s.blend) {
        glEnable(GL_BLEND);
        glBlendFunc(s.srcBlend, s.dstBlend);
    } else {
        glDisable(GL_BLEND);
    }
    if (s.depthOn) {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(s.depthTest ? GL_LEQUAL : GL_ALWAYS);
        glDepthMask(s.depthWrite ? GL_TRUE : GL_FALSE);
    } else {
        glDisable(GL_DEPTH_TEST);
    }
    glBindVertexArray(R.vao);
    glBindBuffer(GL_ARRAY_BUFFER, R.vbo);
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(R.verts.size() * sizeof(GlVertex)), R.verts.data(), GL_STREAM_DRAW);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, GLsizeiptr(R.indices.size() * 4), R.indices.data(), GL_STREAM_DRAW);
    glDrawElements(GL_TRIANGLES, GLsizei(R.indices.size()), GL_UNSIGNED_INT, nullptr);
    glDepthMask(GL_TRUE);
    glBindSampler(0, 0);
    R.verts.clear();
    R.indices.clear();
    R.gpuDirty = true;
    R.drawnSincePresent = true;
}

DrawState stateFor(uint32_t& flags) {
    if (R.current.alphaMap && (flags & 1)) {
        flags |= 0x22;
    }
    uint32_t mode = var(9);
    if (mode == 0) {
        R.dstBlend = GL_ONE_MINUS_SRC_ALPHA;
    } else if (mode == 1) {
        R.dstBlend = GL_ONE;
    }
    DrawState s;
    s.textured = flags & 1;
    s.texture = s.textured ? R.current.id : 0;
    s.blend = flags & 2;
    s.fog = (flags & 8) && !(flags & 0x20);
    s.flat = !(flags & 4);
    s.depthOn = flags & 0xc0;
    s.depthTest = flags & 0x40;
    s.depthWrite = flags & 0x80;
    s.linear = var(24) != 0;
    s.srcBlend = (flags & 0x20) ? GL_SRC_ALPHA : GL_ONE;
    s.dstBlend = R.dstBlend;
    return s;
}

void beginBatch(const DrawState& s) {
    if (!(s == R.batch)) {
        flush();
        R.batch = s;
    }
}

void light(int32_t value, int& intensity, int& spec) {
    int t = (value - 0x100) >> 4;
    if (t > 255) {
        intensity = 255;
        spec = std::min(t - 256, 255);
    } else {
        intensity = std::max(t, 0);
        spec = 0;
    }
}

uint8_t clampByte(int32_t v) {
    return uint8_t(std::clamp(v, 0, 255));
}

float depthValue(int32_t z) {
    int32_t zfar = int32_t(var(20));
    if (zfar <= 0) {
        zfar = 1;
    }
    if (var(26) == 0) {
        float zz = std::clamp(float(z) * 256.0f / float(zfar), 1.0f, 256.0f * R.farScale);
        return 1.0f - 1.0f / zz;
    }
    return std::min(float(z) / (float(zfar) * R.farScale), 1.0f);
}

GlVertex convert(uint32_t v, uint32_t flags, float rhwScale) {
    GlVertex o;
    int32_t z = std::max<int32_t>(int32_t(rd32(g_mem, v + 0x08)), 1);
    o.x = float(int32_t(rd32(g_mem, v + 0x10))) / 65536.0f;
    o.y = float(int32_t(rd32(g_mem, v + 0x14))) / 65536.0f;
    o.z = depthValue(z);
    o.rhw = rhwScale / float(z);
    int32_t u = int32_t(rd32(g_mem, v + 0x18));
    int32_t vv = int32_t(rd32(g_mem, v + 0x1c));
    o.u = float(u) * 5.9604644775390625e-08f;
    o.v = float(vv) * 5.9604644775390625e-08f;
    int32_t r = int32_t(rd32(g_mem, v + 0x20));
    int32_t g = int32_t(rd32(g_mem, v + 0x24));
    int32_t b = int32_t(rd32(g_mem, v + 0x28));
    int32_t a = int32_t(rd32(g_mem, v + 0x2c));
    int intensity = 255;
    int spec = 0;
    if (flags & 4) {
        if (!(flags & 0x200)) {
            light(r, intensity, spec);
        }
    } else if (flags & 0x10) {
        light(int32_t(var(10)), intensity, spec);
    }
    uint8_t fog = (flags & 8) ? clampByte(255 - (a >> 8)) : 255;
    if (flags & 1) {
        uint8_t alpha = (flags & 0x100) ? clampByte(a >> 8) : clampByte(int32_t(var(11)));
        if (flags & 0x200) {
            o.diffuse[0] = clampByte(r >> 8);
            o.diffuse[1] = clampByte(g >> 8);
            o.diffuse[2] = clampByte(b >> 8);
        } else {
            o.diffuse[0] = o.diffuse[1] = o.diffuse[2] = uint8_t(intensity);
        }
        o.diffuse[3] = alpha;
        o.specular[0] = o.specular[1] = o.specular[2] = uint8_t(spec);
    } else {
        if (flags & 0x200) {
            o.diffuse[0] = clampByte(r >> 8);
            o.diffuse[1] = clampByte(g >> 8);
            o.diffuse[2] = clampByte(b >> 8);
        } else {
            uint32_t idx = (flags & 4) ? (uint32_t(u) >> 16) & 0xff : var(12) & 0xff;
            const uint8_t* p = R.palette ? g_mem + R.palette + idx * 3 : nullptr;
            o.diffuse[0] = p ? p[0] : 0;
            o.diffuse[1] = p ? p[1] : 0;
            o.diffuse[2] = p ? p[2] : 0;
        }
        o.diffuse[3] = 255;
        o.specular[0] = o.specular[1] = o.specular[2] = 0;
    }
    o.specular[3] = fog;
    return o;
}

void addFan(const std::vector<uint32_t>& base, size_t first, size_t count) {
    for (size_t i = 1; i + 1 < count; i++) {
        R.indices.push_back(uint32_t(base[first]));
        R.indices.push_back(uint32_t(base[first + i]));
        R.indices.push_back(uint32_t(base[first + i + 1]));
    }
}

struct DrawStats {
    std::unordered_map<uint32_t, uint64_t> polysByFlags;
    std::unordered_map<std::string, uint64_t> callsByKind;
    uint64_t lastPrint = 0;
};

DrawStats g_stats;

void countDraw(const char* kind, uint32_t flags, uint64_t polys) {
    static const bool on = std::getenv("NOC_DRAWSTATS") != nullptr;
    if (!on) {
        return;
    }
    g_stats.polysByFlags[flags] += polys;
    g_stats.callsByKind[kind] += 1;
}

void printDrawStats() {
    static const bool on = std::getenv("NOC_DRAWSTATS") != nullptr;
    if (!on || SDL_GetTicks() - g_stats.lastPrint < 2000) {
        return;
    }
    g_stats.lastPrint = SDL_GetTicks();
    std::fprintf(stderr, "[drawstats] t=%llu calls:", static_cast<unsigned long long>(SDL_GetTicks()));
    for (auto& [k, n] : g_stats.callsByKind) {
        std::fprintf(stderr, " %s=%llu", k.c_str(), static_cast<unsigned long long>(n));
    }
    std::fprintf(stderr, " | polys by flags:");
    for (auto& [f, n] : g_stats.polysByFlags) {
        std::fprintf(stderr, " %03x=%llu", f, static_cast<unsigned long long>(n));
    }
    std::fprintf(stderr, "\n");
    g_stats = DrawStats{};
    g_stats.lastPrint = SDL_GetTicks();
}

uint32_t drawFan(const std::vector<uint32_t>& vptrs, uint32_t flags) {
    countDraw("fan", flags, vptrs.size() >= 3 ? vptrs.size() - 2 : 0);
    static const bool depthStats = std::getenv("NOC_DEPTH_STATS") != nullptr;
    if (depthStats && flags == 0xc0) {
        static std::string lastCamera;
        static int32_t minZ = INT32_MAX, maxZ = INT32_MIN;
        static uint32_t polys = 0;
        std::string cam(reinterpret_cast<const char*>(g_mem + 0x01fb64d8));
        if (cam != lastCamera) {
            if (!lastCamera.empty()) {
                std::fprintf(stderr, "[depthpass] camera %s: %u polys, camera-space z %d..%d\n", lastCamera.c_str(), polys, minZ, maxZ);
            }
            lastCamera = cam;
            minZ = INT32_MAX;
            maxZ = INT32_MIN;
            polys = 0;
        }
        polys++;
        for (uint32_t p : vptrs) {
            int32_t z = int32_t(rd32(g_mem, p + 8));
            minZ = std::min(minZ, z);
            maxZ = std::max(maxZ, z);
        }
    }
    if (!R.inScene || vptrs.size() < 3) {
        return R.inScene ? 1 : 0;
    }
    DrawState s = stateFor(flags);
    beginBatch(s);
    int32_t maxZ = 1;
    for (uint32_t p : vptrs) {
        maxZ = std::max(maxZ, int32_t(rd32(g_mem, p + 0x08)));
    }
    uint32_t start = uint32_t(R.verts.size());
    for (uint32_t p : vptrs) {
        R.verts.push_back(convert(p, flags, float(maxZ)));
    }
    for (uint32_t i = 1; i + 1 < vptrs.size(); i++) {
        R.indices.push_back(start);
        R.indices.push_back(start + i);
        R.indices.push_back(start + i + 1);
    }
    return 1;
}

struct ListVertexCache {
    std::unordered_map<uint64_t, uint32_t> map;
};

uint32_t listVertex(ListVertexCache& cache, uint32_t verts, uint32_t idx, int32_t u, int32_t v, uint32_t flags) {
    uint64_t key = (uint64_t(idx) << 40) ^ (uint64_t(uint32_t(u)) << 20) ^ uint64_t(uint32_t(v));
    uint32_t vp = verts + idx * 0x30;
    wr32(g_mem, vp + 0x18, uint32_t(u));
    wr32(g_mem, vp + 0x1c, uint32_t(v));
    auto it = cache.map.find(key);
    if (it != cache.map.end()) {
        return it->second;
    }
    uint32_t n = uint32_t(R.verts.size());
    R.verts.push_back(convert(vp, flags, 256.0f));
    cache.map.emplace(key, n);
    return n;
}

void endSceneInternal() {
    setVar(11, 0xff);
    setVar(9, 0);
    flush();
    R.inScene = false;
}

uint32_t pixelToBgrx(uint32_t px) {
    if (R.bpp == 16) {
        uint32_t r = (px >> 11) & 31, g = (px >> 5) & 63, b = px & 31;
        return ((r * 255 / 31) << 16) | ((g * 255 / 63) << 8) | (b * 255 / 31);
    }
    return px & 0xffffff;
}

uint32_t bgrxToPixel(uint32_t c) {
    if (R.bpp == 16) {
        uint32_t r = (c >> 16) & 0xff, g = (c >> 8) & 0xff, b = c & 0xff;
        return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
    }
    return c & 0xffffff;
}

uint32_t mirrorPixel(size_t i) {
    return R.bpp == 16 ? rd16(g_mem, R.mirror + uint32_t(i) * 2) : rd32(g_mem, R.mirror + uint32_t(i) * 4);
}

void readBackMirror() {
    glBindFramebuffer(GL_READ_FRAMEBUFFER, R.fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, R.downFbo);
    glBlitFramebuffer(0, 0, R.renderW, R.renderH, 0, 0, R.gameW, R.gameH, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, R.downFbo);
    R.scratch.resize(size_t(R.gameW) * R.gameH);
    glReadPixels(0, 0, R.gameW, R.gameH, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, R.scratch.data());
    for (size_t i = 0; i < R.scratch.size(); i++) {
        uint32_t px = bgrxToPixel(R.scratch[i]);
        if (R.bpp == 16) {
            wr16(g_mem, R.mirror + uint32_t(i) * 2, uint16_t(px));
        } else {
            wr32(g_mem, R.mirror + uint32_t(i) * 4, px);
        }
    }
    R.gpuDirty = false;
}

void drawOverlay(GLuint texture, int w, int h, const uint32_t* pixels);

void compositeMirror(bool replaceAll) {
    size_t n = size_t(R.gameW) * R.gameH;
    size_t bytes = n * size_t(bytesPerPixel());
    if (!replaceAll && std::memcmp(g_mem + R.mirror, R.snapshot.data(), bytes) == 0) {
        return;
    }
    R.scratch.resize(n);
    int bpp = bytesPerPixel();
    for (size_t i = 0; i < n; i++) {
        uint32_t now = mirrorPixel(i);
        uint32_t before = 0;
        std::memcpy(&before, R.snapshot.data() + i * size_t(bpp), size_t(bpp));
        R.scratch[i] = (replaceAll || now != before ? 0xff000000u : 0u) | pixelToBgrx(now);
    }
    drawOverlay(R.overlayTex, R.gameW, R.gameH, R.scratch.data());
}

void drawOverlay(GLuint texture, int w, int h, const uint32_t* pixels) {
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, pixels);
    bindTarget();
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glUseProgram(R.overlayProgram);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);
    glBindVertexArray(R.emptyVao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    R.gpuDirty = true;
}

void uploadTexture(Texture& t, uint32_t size, uint32_t pixels, uint32_t pal, uint32_t alpha) {
    std::vector<uint32_t> rgba(size_t(size) * size);
    for (size_t i = 0; i < rgba.size(); i++) {
        uint8_t idx = g_mem[pixels + i];
        const uint8_t* c = g_mem + pal + idx * 3;
        uint8_t a = alpha ? g_mem[alpha + i] : ((c[0] | c[1] | c[2]) ? 255 : 0);
        rgba[i] = uint32_t(c[0]) | (uint32_t(c[1]) << 8) | (uint32_t(c[2]) << 16) | (uint32_t(a) << 24);
    }
    if (!t.id) {
        glGenTextures(1, &t.id);
    }
    t.alphaMap = alpha != 0;
    glBindTexture(GL_TEXTURE_2D, t.id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, int(size), int(size), 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
}

uint32_t selectOrUpdate(const HleArgs& a, bool update) {
    uint32_t size = var(18);
    if (size != 32 && size != 64 && size != 128 && size != 256) {
        if (!update) {
            R.current = Texture{};
        }
        return 1;
    }
    std::string key = std::string(gstr(a[0] + 8)) + "#" + std::to_string(size);
    auto it = R.textures.find(key);
    if (it != R.textures.end()) {
        if (update) {
            flush();
            uploadTexture(it->second, size, a[2], a[3], a[4]);
        }
        R.current = it->second;
        return 1;
    }
    Texture& t = R.textures[key];
    uploadTexture(t, size, a[2], a[3], a[4]);
    R.current = t;
    return 1;
}

uint32_t setVideoMode(uint32_t rowTable) {
    if (!display_init()) {
        return 0;
    }
    if (!R.ready) {
        SDL_Window* win = user32_window();
        if (!std::getenv("NOC_WINDOWED")) {
            SDL_SetWindowFullscreen(win, true);
            SDL_SyncWindow(win);
        }
        createPipeline();
        createTargets();
        R.mirror = guest_alloc(uint32_t(R.gameW * R.gameH * 4), 16);
        R.snapshot.resize(size_t(R.gameW) * R.gameH * 4);
        R.ready = true;
    }
    R.rowTable = rowTable;
    R.savedRows.resize(size_t(R.gameH));
    for (int y = 0; y < R.gameH; y++) {
        R.savedRows[size_t(y)] = rd32(g_mem, rowTable + 4 * uint32_t(y));
    }
    bindTarget();
    glClearColor(0, 0, 0, 1);
    glClearDepth(1.0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    R.gpuDirty = true;
    SDL_HideCursor();
    return 1;
}

uint32_t guestString(const char* s) {
    return guest_strdup(s);
}

}

bool renderer_world3d_begin(World3dView& view) {
    if (!R.ready) {
        return false;
    }
    flush();
    view.zfar = float(std::max<int32_t>(int32_t(var(20)), 1));
    view.linearDepth = var(26) != 0;
    view.farScale = kWorld3dFarScale;
    if (!world3d_render(R.fbo, R.renderW, R.renderH, view)) {
        return false;
    }
    R.farScale = kWorld3dFarScale;
    size_t bytes = size_t(R.gameW) * R.gameH * size_t(bytesPerPixel());
    std::memset(g_mem + R.mirror, 0, bytes);
    std::memset(R.snapshot.data(), 0, bytes);
    R.inScene = true;
    R.gpuDirty = true;
    R.drawnSincePresent = true;
    return true;
}

void renderer_world3d_end() {
    if (R.inScene) {
        endSceneInternal();
    }
    R.farScale = 1.0f;
}

HLE_CDECL("tridx6", APIDLLInformation, 2) {
    uint32_t out = a[1];
    std::memset(g_mem + out, 0, 0x1dc8);
    copy_string_out(out, 0x100, "Native OpenGL renderer");
    copy_string_out(out + 0x100, 0x100, "Terminal Reality Inc.");
    wr16(m, out + 0x200, 0x100);
    wr16(m, out + 0x202, 0x100);
    wr16(m, out + 0x204, 0xffff);
    wr32(m, out + 0x208, 3);
    wr32(m, out + 0x20c, 0x10);
    return 0;
}

HLE_CDECL("tridx6", APIDLLinit, 2) {
    for (int i = 0; i < 35; i++) {
        R.table[i] = rd32(m, a[1] + 4 * uint32_t(i));
    }
    return 1;
}

HLE_CDECL("tridx6", APIDLLkill, 0) { return 1; }
HLE_CDECL("tridx6", APIDLLselectCard, 1) { return 1; }

HLE_CDECL("tridx6", APIDLLbuildCardList, 5) {
    static uint32_t driver = guestString("opengl");
    static uint32_t desc = guestString("Native OpenGL renderer");
    wr32(m, a[0], 1);
    wr32(m, a[1], driver);
    wr32(m, a[2], desc);
    wr32(m, a[3], 0);
    wr32(m, a[4], 0);
    return 1;
}

HLE_CDECL("tridx6", APIDLLgetVideoMemory, 3) {
    if (!a[0] || !a[1] || !a[2]) {
        return 0;
    }
    wr32(m, a[0], 256u << 20);
    wr32(m, a[1], 256u << 20);
    wr32(m, a[2], 0);
    return 1;
}

HLE_CDECL("tridx6", APIDLLsetVideoMode, 1) { return setVideoMode(a[0]); }

HLE_CDECL("tridx6", APIDLLsetVideoMode2, 4) {
    R.gameW = int(a[0]);
    R.gameH = int(a[1]);
    R.bpp = int(a[2]) == 16 ? 16 : 32;
    return setVideoMode(a[3]);
}

HLE_CDECL("tridx6", APIDLLrestoreVideoMode, 0) {
    if (R.frameLocked) {
        for (int y = 0; y < R.gameH; y++) {
            wr32(m, R.rowTable + 4 * uint32_t(y), R.savedRows[size_t(y)]);
        }
        R.frameLocked = false;
    }
    SDL_ShowCursor();
    return 1;
}

HLE_CDECL("tridx6", APIDLLbeginScene, 0) {
    if (R.inScene) {
        return 0;
    }
    R.inScene = true;
    return 1;
}

HLE_CDECL("tridx6", APIDLLendScene, 0) {
    if (!R.inScene) {
        return 0;
    }
    endSceneInternal();
    return 1;
}

HLE_CDECL("tridx6", APIDLLtoggle, 0) {
    if (R.frameLocked || !R.ready) {
        return 1;
    }
    flush();
    display_present_texture(R.color, R.renderW, R.renderH);
    R.drawnSincePresent = false;
    printDrawStats();
    return 1;
}

HLE_CDECL("tridx6", APIDLLsync, 0) {
    flush();
    return 1;
}

HLE_CDECL("tridx6", APIDLLclear, 0) {
    flush();
    bindTarget();
    glDisable(GL_SCISSOR_TEST);
    if (R.inScene) {
        setVar(12, 0);
        const uint8_t* p = R.palette ? g_mem + R.palette : nullptr;
        glClearColor(p ? p[0] / 255.0f : 0, p ? p[1] / 255.0f : 0, p ? p[2] / 255.0f : 0, 1);
    } else {
        glClearColor(0, 0, 0, 1);
    }
    glClear(GL_COLOR_BUFFER_BIT);
    R.gpuDirty = true;
    return 1;
}

HLE_CDECL("tridx6", APIDLLclearZBuffer, 0) {
    flush();
    bindTarget();
    glDisable(GL_SCISSOR_TEST);
    glDepthMask(GL_TRUE);
    glClearDepth(1.0);
    glClear(GL_DEPTH_BUFFER_BIT);
    return 1;
}

HLE_CDECL("tridx6", APIDLLclearZBox, 4) {
    flush();
    bindTarget();
    int x0 = int32_t(a[0]) * R.renderW / R.gameW;
    int x1 = (int32_t(a[1]) + 1) * R.renderW / R.gameW;
    int y0 = int32_t(a[2]) * R.renderH / R.gameH;
    int y1 = (int32_t(a[3]) + 1) * R.renderH / R.gameH;
    glEnable(GL_SCISSOR_TEST);
    glScissor(x0, y0, std::max(0, x1 - x0), std::max(0, y1 - y0));
    glDepthMask(GL_TRUE);
    glClearDepth(1.0);
    glClear(GL_DEPTH_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);
    return 1;
}

HLE_CDECL("tridx6", APIDLLmasterZBuffer, 1) {
    uint32_t i = a[0];
    if (i >= R.masterFbo.size()) {
        return 0;
    }
    flush();
    glBindFramebuffer(GL_READ_FRAMEBUFFER, R.fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, R.masterFbo[i]);
    glBlitFramebuffer(0, 0, R.renderW, R.renderH, 0, 0, R.renderW, R.renderH, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
    return 1;
}

HLE_CDECL("tridx6", APIDLLrestoreZBuffer, 5) {
    uint32_t i = a[0];
    if (i >= R.masterFbo.size()) {
        return 0;
    }
    flush();
    int x0 = int32_t(a[1]) * R.renderW / R.gameW;
    int y0 = int32_t(a[2]) * R.renderH / R.gameH;
    int x1 = (int32_t(a[3]) + 1) * R.renderW / R.gameW;
    int y1 = (int32_t(a[4]) + 1) * R.renderH / R.gameH;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, R.masterFbo[i]);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, R.fbo);
    glBlitFramebuffer(x0, y0, x1, y1, x0, y0, x1, y1, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
    return 1;
}

HLE_CDECL("tridx6", APIDLLlockFrame, 0) {
    if (!R.ready || R.frameLocked) {
        return 0;
    }
    logFirst("lockFrame");
    if (R.inScene) {
        endSceneInternal();
    }
    flush();
    if (R.gpuDirty) {
        readBackMirror();
    }
    size_t bytes = size_t(R.gameW) * R.gameH * size_t(bytesPerPixel());
    std::memcpy(R.snapshot.data(), g_mem + R.mirror, bytes);
    uint32_t pitch = uint32_t(R.gameW * bytesPerPixel());
    for (int y = 0; y < R.gameH; y++) {
        wr32(m, R.rowTable + 4 * uint32_t(y), R.mirror + uint32_t(y) * pitch);
    }
    R.frameLocked = true;
    return 1;
}

HLE_CDECL("tridx6", APIDLLunlockFrame, 0) {
    if (!R.frameLocked) {
        return 0;
    }
    for (int y = 0; y < R.gameH; y++) {
        wr32(m, R.rowTable + 4 * uint32_t(y), R.savedRows[size_t(y)]);
    }
    compositeMirror(!R.drawnSincePresent);
    R.frameLocked = false;
    return 1;
}

HLE_CDECL("tridx6", APIDLLlockHoldBuffer, 0) { return 0; }
HLE_CDECL("tridx6", APIDLLunlockHoldBuffer, 0) { return 0; }

HLE_CDECL("tridx6", APIDLLsetColorTable16, 2) {
    R.palette = a[0];
    uint32_t shift[3], div[3], log2v[3];
    if (R.bpp == 16) {
        uint32_t s[3] = {11, 5, 0}, d[3] = {8, 4, 8}, l[3] = {3, 2, 3};
        std::copy(s, s + 3, shift);
        std::copy(d, d + 3, div);
        std::copy(l, l + 3, log2v);
    } else {
        uint32_t s[3] = {16, 8, 0};
        std::copy(s, s + 3, shift);
        std::fill(div, div + 3, 1u);
        std::fill(log2v, log2v + 3, 0u);
    }
    for (int ch = 0; ch < 3; ch++) {
        setVar(ch * 3 + 0, shift[ch]);
        setVar(ch * 3 + 1, div[ch]);
        setVar(ch * 3 + 2, log2v[ch]);
    }
    if (a[1]) {
        for (uint32_t i = 0; i < 256; i++) {
            const uint8_t* c = g_mem + a[0] + i * 3;
            uint32_t v = ((c[0] / div[0]) << shift[0]) | ((c[1] / div[1]) << shift[1]) | ((c[2] / div[2]) << shift[2]);
            wr16(m, a[1] + 2 * i, uint16_t(v));
        }
    }
    return 1;
}

HLE_CDECL("tridx6", APIDLLsetFogColor, 3) {
    flush();
    R.fogColor[0] = float(a[0] & 0xff) / 255.0f;
    R.fogColor[1] = float(a[1] & 0xff) / 255.0f;
    R.fogColor[2] = float(a[2] & 0xff) / 255.0f;
    endSceneInternal();
    return 1;
}

HLE_CDECL("tridx6", APIDLLselectTexture, 5) { return selectOrUpdate(a, false); }
HLE_CDECL("tridx6", APIDLLupdateTexture, 5) { return selectOrUpdate(a, true); }
HLE_CDECL("tridx6", APIDLLsetMipMapLevel, 0) { return 1; }

HLE_CDECL("tridx6", APIDLLdrawPolygon, 3) {
    std::vector<uint32_t> ptrs(a[1]);
    for (uint32_t i = 0; i < a[1]; i++) {
        ptrs[i] = a[0] + i * 0x30;
    }
    return drawFan(ptrs, a[2]);
}

HLE_CDECL("tridx6", APIDLLdrawPolygon2, 3) {
    std::vector<uint32_t> ptrs(a[1]);
    for (uint32_t i = 0; i < a[1]; i++) {
        ptrs[i] = rd32(m, a[0] + 4 * i);
    }
    return drawFan(ptrs, a[2]);
}

HLE_CDECL("tridx6", APIDLLdrawPolyList, 4) {
    countDraw("list", a[3], a[2]);
    if (!R.inScene) {
        return 0;
    }
    uint32_t flags = a[3];
    beginBatch(stateFor(flags));
    ListVertexCache cache;
    for (uint32_t f = 0; f < a[2]; f++) {
        uint32_t face = rd32(m, a[1] + 4 * f);
        uint32_t n = rd32(m, face + 4);
        std::vector<uint32_t> idx(n);
        for (uint32_t k = 0; k < n; k++) {
            uint32_t e = face + 0x18 + 12 * k;
            idx[k] = listVertex(cache, a[0], rd32(m, e), int32_t(rd32(m, e + 4)), int32_t(rd32(m, e + 8)), flags);
        }
        addFan(idx, 0, n);
    }
    flush();
    return 1;
}

HLE_CDECL("tridx6", APIDLLdrawPolyList2, 4) {
    countDraw("list2", a[3], a[2]);
    if (!R.inScene) {
        return 0;
    }
    uint32_t flags = a[3];
    beginBatch(stateFor(flags));
    ListVertexCache cache;
    for (uint32_t t = 0; t < a[2]; t++) {
        uint32_t tri = rd32(m, a[1] + 4 * t);
        for (uint32_t k = 0; k < 3; k++) {
            uint32_t vi = rd16(m, tri + 2 * k);
            int32_t u = int32_t(uint32_t(rd16(m, tri + 6 + 2 * k)) << 8);
            int32_t v = int32_t(uint32_t(rd16(m, tri + 12 + 2 * k)) << 8);
            R.indices.push_back(listVertex(cache, a[0], vi, u, v, flags));
        }
    }
    flush();
    return 1;
}

HLE_CDECL("tridx6", APIDLLaddParticle, 0) { return 0; }
HLE_CDECL("tridx6", APIDLLflushParticleList, 0) { return 0; }
HLE_CDECL("tridx6", APIDLLadd3dLine, 0) { return 0; }
HLE_CDECL("tridx6", APIDLLflushLineList, 0) { return 0; }
