#include "projection.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace {

constexpr int kBakeTile = 1024;
constexpr int kIdScale = 2;

const char* kDepthVS = R"(#version 330 core
layout(location = 0) in vec3 pos;
layout(location = 1) in vec3 normal;
uniform vec3 camPos;
uniform mat3 camRot;
uniform vec2 camF;
uniform vec2 halfSize;
out float viewDepth;
out float facing;
void main() {
    vec3 d = pos - camPos;
    vec3 c = vec3(dot(camRot[0], d), dot(camRot[1], d), dot(camRot[2], d));
    const float near = 0.5;
    const float far = 4000.0;
    float zc = c.z * (far + near) / (far - near) - 2.0 * far * near / (far - near);
    gl_Position = vec4(camF.x * c.x / halfSize.x, -camF.y * c.y / halfSize.y, zc, c.z);
    viewDepth = c.z;
    facing = dot(normal, -d);
}
)";

const char* kDepthFS = R"(#version 330 core
in float viewDepth;
in float facing;
out float depth;
void main() {
    if (facing <= 0.0) {
        discard;
    }
    depth = viewDepth;
}
)";

const char* kIdFS = R"(#version 330 core
in float viewDepth;
in float facing;
out uint id;
void main() {
    if (facing <= 0.0) {
        discard;
    }
    id = uint(gl_PrimitiveID + 1);
}
)";

const char* kBakeVS = R"(#version 330 core
layout(location = 0) in vec3 pos;
layout(location = 1) in vec3 normal;
layout(location = 3) in vec3 uvPage;
uniform float page;
uniform float pageSize;
out vec3 worldPos;
out vec3 worldNormal;
void main() {
    worldPos = pos;
    worldNormal = normal;
    gl_Position = uvPage.z == page ? vec4(uvPage.xy / pageSize * 2.0 - 1.0, 0.0, 1.0) : vec4(2.0, 2.0, 2.0, 1.0);
}
)";

const char* kBakeFS = R"(
in vec3 worldPos;
in vec3 worldNormal;
layout(location = 0) out vec4 color;
layout(location = 1) out vec4 position;
layout(location = 2) out vec4 normal;
void main() {
    vec3 n = normalize(worldNormal);
    vec3 c;
    int count = projectColor(worldPos, n, -1, c);
    color = count > 0 ? vec4(c, 1.0) : vec4(0.0, 0.0, 0.0, 1.0 / 255.0);
    position = vec4(worldPos, length(cross(dFdx(worldPos), dFdy(worldPos))));
    normal = vec4(n * 0.5 + 0.5, 1.0);
}
)";

const char* kFillFS = R"(#version 330 core
uniform sampler3D gridPX;
uniform sampler3D gridNX;
uniform sampler3D gridPY;
uniform sampler3D gridNY;
uniform sampler3D gridPZ;
uniform sampler3D gridNZ;
uniform sampler3D gridAll;
uniform vec3 gridOrigin;
uniform vec3 gridExtent;
uniform float gridVoxel;
uniform int gridLevels;
uniform vec3 gridBase[7];
in vec3 worldPos;
in vec3 worldNormal;
out vec4 color;
vec4 pullGrid(sampler3D g, vec3 uvw, vec3 base) {
    vec3 c = base;
    float conf = 0.0;
    for (int l = gridLevels - 1; l >= 0; l--) {
        vec4 s = textureLod(g, uvw, float(l));
        if (s.a > 0.0) {
            float w = clamp(s.a / (0.25 * gridVoxel * gridVoxel / exp2(float(l))), 0.0, 1.0);
            c = mix(c, s.rgb / s.a, w);
            conf = max(conf, w);
        }
    }
    return vec4(c, conf);
}
void main() {
    vec3 n = normalize(worldNormal);
    vec3 uvw = (worldPos - gridOrigin) / gridExtent;
    vec4 cx;
    vec4 cy;
    vec4 cz;
    if (n.x >= 0.0) {
        cx = pullGrid(gridPX, uvw, gridBase[0]);
    } else {
        cx = pullGrid(gridNX, uvw, gridBase[1]);
    }
    if (n.y >= 0.0) {
        cy = pullGrid(gridPY, uvw, gridBase[2]);
    } else {
        cy = pullGrid(gridNY, uvw, gridBase[3]);
    }
    if (n.z >= 0.0) {
        cz = pullGrid(gridPZ, uvw, gridBase[4]);
    } else {
        cz = pullGrid(gridNZ, uvw, gridBase[5]);
    }
    vec4 ca = pullGrid(gridAll, uvw, gridBase[6]);
    vec3 w = n * n * vec3(cx.a, cy.a, cz.a);
    float ws = w.x + w.y + w.z;
    vec3 directional = (w.x * cx.rgb + w.y * cy.rgb + w.z * cz.rgb) / max(ws, 1e-4);
    color = vec4(mix(ca.rgb, directional, clamp(2.0 * ws, 0.0, 1.0)), 1.0);
}
)";

GLuint g_depthProgram = 0;
GLuint g_idProgram = 0;
GLuint g_bakeProgram = 0;
GLuint g_fillProgram = 0;

constexpr int kGridGrids = 7;
constexpr int kGridLevels = 6;
constexpr double kGridMaxVoxels = 1.5e6;

struct FillGrid {
    float origin[3];
    float voxel;
    int dims[3];
    std::vector<float> acc[kGridGrids];
    double base[kGridGrids][4] = {};
};

FillGrid makeGrid(const ProjectionGpu& gpu) {
    FillGrid g;
    double volume = 1;
    float extent[3];
    for (int a = 0; a < 3; a++) {
        extent[a] = gpu.boundsMax[a] - gpu.boundsMin[a] + 2.0f;
        volume *= extent[a];
    }
    g.voxel = std::max(2.0f, float(std::cbrt(volume / kGridMaxVoxels)));
    int multiple = 1 << (kGridLevels - 1);
    for (int a = 0; a < 3; a++) {
        g.origin[a] = gpu.boundsMin[a] - 1.0f;
        g.dims[a] = (int(std::ceil(extent[a] / g.voxel)) + multiple - 1) / multiple * multiple;
    }
    for (std::vector<float>& acc : g.acc) {
        acc.assign(size_t(g.dims[0]) * size_t(g.dims[1]) * size_t(g.dims[2]) * 4, 0.0f);
    }
    return g;
}

void splat(FillGrid& g, const std::vector<uint32_t>& color, const std::vector<float>& position, const std::vector<uint32_t>& normal) {
    for (size_t i = 0; i < color.size(); i++) {
        if ((color[i] >> 24) != 255) {
            continue;
        }
        int v[3];
        for (int a = 0; a < 3; a++) {
            v[a] = std::clamp(int((position[i * 4 + size_t(a)] - g.origin[a]) / g.voxel), 0, g.dims[a] - 1);
        }
        size_t cell = (size_t(v[2]) * size_t(g.dims[1]) + size_t(v[1])) * size_t(g.dims[0]) + size_t(v[0]);
        float texelArea = position[i * 4 + 3];
        float rgb[3] = {float(color[i] & 0xff) / 255.0f, float((color[i] >> 8) & 0xff) / 255.0f, float((color[i] >> 16) & 0xff) / 255.0f};
        auto add = [&](int grid, float w) {
            float* a = g.acc[grid].data() + cell * 4;
            for (int k = 0; k < 3; k++) {
                a[k] += rgb[k] * w;
                g.base[grid][k] += double(rgb[k] * w);
            }
            a[3] += w;
            g.base[grid][3] += double(w);
        };
        for (int a = 0; a < 3; a++) {
            float n = float((normal[i] >> (8 * a)) & 0xff) / 127.5f - 1.0f;
            if (n * n > 1e-3f) {
                add(2 * a + (n < 0.0f ? 1 : 0), n * n * texelArea);
            }
        }
        add(6, texelArea);
    }
}

void uploadGrid(const FillGrid& g, GLuint textures[kGridGrids]) {
    glGenTextures(kGridGrids, textures);
    for (int t = 0; t < kGridGrids; t++) {
        glBindTexture(GL_TEXTURE_3D, textures[t]);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_NEAREST);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAX_LEVEL, kGridLevels - 1);
        std::vector<float> level = g.acc[t];
        int d[3] = {g.dims[0], g.dims[1], g.dims[2]};
        for (int l = 0; l < kGridLevels; l++) {
            glTexImage3D(GL_TEXTURE_3D, l, GL_RGBA16F, d[0], d[1], d[2], 0, GL_RGBA, GL_FLOAT, level.data());
            if (l + 1 == kGridLevels) {
                break;
            }
            int h[3] = {d[0] / 2, d[1] / 2, d[2] / 2};
            std::vector<float> coarse(size_t(h[0]) * size_t(h[1]) * size_t(h[2]) * 4, 0.0f);
            for (int z = 0; z < d[2]; z++) {
                for (int y = 0; y < d[1]; y++) {
                    for (int x = 0; x < d[0]; x++) {
                        const float* src = level.data() + ((size_t(z) * size_t(d[1]) + size_t(y)) * size_t(d[0]) + size_t(x)) * 4;
                        float* dst = coarse.data() + ((size_t(z / 2) * size_t(h[1]) + size_t(y / 2)) * size_t(h[0]) + size_t(x / 2)) * 4;
                        for (int k = 0; k < 4; k++) {
                            dst[k] += src[k] * 0.125f;
                        }
                    }
                }
            }
            level = std::move(coarse);
            std::copy_n(h, 3, d);
        }
    }
}

GLuint compile(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetShaderInfoLog(s, sizeof log, nullptr, log);
        std::fprintf(stderr, "[projection] shader compile failed: %s\n", log);
    }
    return s;
}

void initPrograms() {
    if (g_depthProgram) {
        return;
    }
    g_depthProgram = projection_link(kDepthVS, kDepthFS);
    g_idProgram = projection_link(kDepthVS, kIdFS);
    std::string bake = std::string("#version 330 core\n") + kProjectionGlsl + kBakeFS;
    g_bakeProgram = projection_link(kBakeVS, bake.c_str());
    g_fillProgram = projection_link(kBakeVS, kFillFS);
}

void setCameraUniforms(GLuint program, const SceneCamera& cam) {
    glUniform3fv(glGetUniformLocation(program, "camPos"), 1, cam.pos);
    glUniformMatrix3fv(glGetUniformLocation(program, "camRot"), 1, GL_TRUE, cam.m);
    glUniform2f(glGetUniformLocation(program, "camF"), cam.f, cam.f);
    glUniform2f(glGetUniformLocation(program, "halfSize"), float(kBackdropW / 2), float(kBackdropH / 2));
}

GLuint makeArray(GLenum internal, int w, int h, int layers, GLenum filter) {
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D_ARRAY, tex);
    GLenum format = internal == GL_R32F ? GL_RED : GL_RGBA;
    GLenum type = internal == GL_R32F ? GL_FLOAT : GL_UNSIGNED_BYTE;
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GLint(internal), w, h, std::max(layers, 1), 0, format, type, nullptr);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GLint(filter));
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GLint(filter));
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return tex;
}

}

const char* const kProjectionGlsl = R"(
uniform samplerBuffer cameras;
uniform int camCount;
uniform sampler2DArray backdrops;
uniform sampler2DArray depthMaps;
const float kSaturation = 40.0;
const float kAgree = 0.12;
const int kMaxCandidates = 32;
bool cameraSample(int i, vec3 p, vec3 n, out vec2 uv, out float q) {
    vec4 right = texelFetch(cameras, 4 * i);
    vec4 up = texelFetch(cameras, 4 * i + 1);
    vec4 forward = texelFetch(cameras, 4 * i + 2);
    vec4 posF = texelFetch(cameras, 4 * i + 3);
    uv = vec2(0.0);
    q = 0.0;
    vec3 d = p - posF.xyz;
    float cz = dot(forward.xyz, d);
    if (cz < 0.5) {
        return false;
    }
    float sx = 320.0 + posF.w * dot(right.xyz, d) / cz;
    float sy = 240.0 - posF.w * dot(up.xyz, d) / cz;
    if (sx < 0.5 || sx > 639.5 || sy < 0.5 || sy > 479.5) {
        return false;
    }
    uv = vec2(sx / 640.0, sy / 480.0);
    if (cz > texture(depthMaps, vec3(uv, float(i))).r * 1.01 + 0.15) {
        return false;
    }
    float facing = dot(n, -normalize(d));
    if (facing < 0.05) {
        return false;
    }
    q = min(posF.w * facing / cz, kSaturation) * (0.5 + 0.5 * facing);
    return true;
}
int projectColor(vec3 p, vec3 n, int exclude, out vec3 color) {
    color = vec3(0.0);
    vec2 uv;
    float q;
    float qmax = 0.0;
    for (int i = 0; i < camCount; i++) {
        if (i != exclude && cameraSample(i, p, n, uv, q)) {
            qmax = max(qmax, q);
        }
    }
    if (qmax == 0.0) {
        return 0;
    }
    vec3 cols[kMaxCandidates];
    float qs[kMaxCandidates];
    int count = 0;
    for (int i = 0; i < camCount && count < kMaxCandidates; i++) {
        if (i != exclude && cameraSample(i, p, n, uv, q) && q >= 0.5 * qmax) {
            cols[count] = texture(backdrops, vec3(uv, float(i))).rgb;
            qs[count] = q;
            count++;
        }
    }
    int ref = 0;
    float bestSupport = -1.0;
    for (int j = 0; j < count; j++) {
        float support = 0.5 * qs[j] / qmax;
        for (int k = 0; k < count; k++) {
            if (distance(cols[j], cols[k]) < kAgree) {
                support += 1.0;
            }
        }
        if (support > bestSupport) {
            bestSupport = support;
            ref = j;
        }
    }
    vec3 sum = vec3(0.0);
    float wsum = 0.0;
    for (int k = 0; k < count; k++) {
        if (distance(cols[k], cols[ref]) >= kAgree) {
            continue;
        }
        float w = pow(qs[k] / qmax, 4.0);
        sum += cols[k] * w;
        wsum += w;
    }
    color = sum / wsum;
    return count;
}
)";

GLuint projection_link(const char* vs, const char* fs) {
    GLuint p = glCreateProgram();
    glAttachShader(p, compile(GL_VERTEX_SHADER, vs));
    glAttachShader(p, compile(GL_FRAGMENT_SHADER, fs));
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetProgramInfoLog(p, sizeof log, nullptr, log);
        std::fprintf(stderr, "[projection] program link failed: %s\n", log);
    }
    return p;
}

GLuint projection_depth_program() {
    initPrograms();
    return g_depthProgram;
}

bool projection_create(const Scene& scene, ProjectionGpu& gpu) {
    initPrograms();
    gpu = ProjectionGpu{};
    gpu.vertexCount = GLsizei(scene.verts.size() / kSceneVertexFloats);
    gpu.cameraCount = GLsizei(scene.cameras.size());
    for (int a = 0; a < 3; a++) {
        gpu.boundsMin[a] = 1e30f;
        gpu.boundsMax[a] = -1e30f;
    }
    for (size_t i = 0; i < scene.verts.size(); i += kSceneVertexFloats) {
        for (int a = 0; a < 3; a++) {
            gpu.boundsMin[a] = std::min(gpu.boundsMin[a], scene.verts[i + size_t(a)]);
            gpu.boundsMax[a] = std::max(gpu.boundsMax[a], scene.verts[i + size_t(a)]);
        }
    }
    glGenVertexArrays(1, &gpu.vao);
    glBindVertexArray(gpu.vao);
    glGenBuffers(1, &gpu.vbo);
    glBindBuffer(GL_ARRAY_BUFFER, gpu.vbo);
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(scene.verts.size() * 4), scene.verts.data(), GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, kSceneVertexFloats * 4, nullptr);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, kSceneVertexFloats * 4, reinterpret_cast<void*>(12));
    glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, kSceneVertexFloats * 4, reinterpret_cast<void*>(24));
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);

    gpu.backdrops = makeArray(GL_RGBA8, kBackdropW, kBackdropH, gpu.cameraCount, GL_LINEAR);
    std::vector<uint32_t> rgba;
    std::vector<float> cams(size_t(gpu.cameraCount) * 16, 0.0f);
    for (GLsizei i = 0; i < gpu.cameraCount; i++) {
        const SceneCamera& cam = scene.cameras[size_t(i)];
        scene_backdrop(cam.name, rgba);
        glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, i, kBackdropW, kBackdropH, 1, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
        float* u = cams.data() + size_t(i) * 16;
        for (int r = 0; r < 3; r++) {
            u[0 + r] = cam.m[r * 3 + 0];
            u[4 + r] = cam.m[r * 3 + 1];
            u[8 + r] = cam.m[r * 3 + 2];
        }
        std::copy_n(cam.pos, 3, u + 12);
        u[15] = cam.f;
    }
    glGenBuffers(1, &gpu.cameraBuffer);
    glBindBuffer(GL_TEXTURE_BUFFER, gpu.cameraBuffer);
    glBufferData(GL_TEXTURE_BUFFER, GLsizeiptr(cams.size() * 4), cams.data(), GL_STATIC_DRAW);
    glGenTextures(1, &gpu.cameraTex);
    glBindTexture(GL_TEXTURE_BUFFER, gpu.cameraTex);
    glTexBuffer(GL_TEXTURE_BUFFER, GL_RGBA32F, gpu.cameraBuffer);

    gpu.depthMaps = makeArray(GL_R32F, kBackdropW, kBackdropH, gpu.cameraCount, GL_NEAREST);
    GLuint fbo = 0, depth = 0;
    glGenFramebuffers(1, &fbo);
    glGenRenderbuffers(1, &depth);
    glBindRenderbuffer(GL_RENDERBUFFER, depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT32F, kBackdropW, kBackdropH);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth);
    glViewport(0, 0, kBackdropW, kBackdropH);
    glUseProgram(g_depthProgram);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    const float far[4] = {1e9f, 1e9f, 1e9f, 1e9f};
    for (GLsizei i = 0; i < gpu.cameraCount; i++) {
        setCameraUniforms(g_depthProgram, scene.cameras[size_t(i)]);
        glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, gpu.depthMaps, 0, i);
        glClearBufferfv(GL_COLOR, 0, far);
        glClear(GL_DEPTH_BUFFER_BIT);
        glDrawArrays(GL_TRIANGLES, 0, gpu.vertexCount);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);
    glDeleteRenderbuffers(1, &depth);
    return gpu.vertexCount > 0 && gpu.cameraCount > 0;
}

void projection_destroy(ProjectionGpu& gpu) {
    glDeleteVertexArrays(1, &gpu.vao);
    glDeleteBuffers(1, &gpu.vbo);
    glDeleteBuffers(1, &gpu.uvVbo);
    glDeleteTextures(1, &gpu.backdrops);
    glDeleteTextures(1, &gpu.depthMaps);
    glDeleteTextures(1, &gpu.cameraTex);
    glDeleteBuffers(1, &gpu.cameraBuffer);
    gpu = ProjectionGpu{};
}

void projection_bind(const ProjectionGpu& gpu, GLuint program, int firstUnit) {
    glUniform1i(glGetUniformLocation(program, "camCount"), gpu.cameraCount);
    glUniform1i(glGetUniformLocation(program, "cameras"), firstUnit);
    glUniform1i(glGetUniformLocation(program, "backdrops"), firstUnit + 1);
    glUniform1i(glGetUniformLocation(program, "depthMaps"), firstUnit + 2);
    glActiveTexture(GLenum(GL_TEXTURE0 + firstUnit));
    glBindTexture(GL_TEXTURE_BUFFER, gpu.cameraTex);
    glActiveTexture(GLenum(GL_TEXTURE0 + firstUnit + 1));
    glBindTexture(GL_TEXTURE_2D_ARRAY, gpu.backdrops);
    glActiveTexture(GLenum(GL_TEXTURE0 + firstUnit + 2));
    glBindTexture(GL_TEXTURE_2D_ARRAY, gpu.depthMaps);
    glActiveTexture(GL_TEXTURE0);
}

std::vector<float> projection_face_density(const ProjectionGpu& gpu, const Scene& scene) {
    initPrograms();
    int w = kBackdropW * kIdScale, h = kBackdropH * kIdScale;
    GLuint fbo = 0, ids = 0, depth = 0;
    glGenTextures(1, &ids);
    glBindTexture(GL_TEXTURE_2D, ids);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R32UI, w, h, 0, GL_RED_INTEGER, GL_UNSIGNED_INT, nullptr);
    glGenRenderbuffers(1, &depth);
    glBindRenderbuffer(GL_RENDERBUFFER, depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT32F, w, h);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, ids, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth);
    glViewport(0, 0, w, h);
    glUseProgram(g_idProgram);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glBindVertexArray(gpu.vao);
    std::vector<float> density(size_t(gpu.vertexCount / 3), 0.0f);
    std::vector<uint8_t> seen(density.size(), 0);
    std::vector<uint32_t> faces;
    std::vector<uint32_t> pixels(size_t(w) * size_t(h));
    const GLuint zero[4] = {0, 0, 0, 0};
    for (const SceneCamera& cam : scene.cameras) {
        setCameraUniforms(g_idProgram, cam);
        glClearBufferuiv(GL_COLOR, 0, zero);
        glClear(GL_DEPTH_BUFFER_BIT);
        glDrawArrays(GL_TRIANGLES, 0, gpu.vertexCount);
        glReadPixels(0, 0, w, h, GL_RED_INTEGER, GL_UNSIGNED_INT, pixels.data());
        faces.clear();
        for (uint32_t id : pixels) {
            if (id && id <= seen.size() && !seen[id - 1]) {
                seen[id - 1] = 1;
                faces.push_back(id - 1);
            }
        }
        for (uint32_t face : faces) {
            seen[face] = 0;
            const float* v = scene.verts.data() + size_t(face) * 3 * kSceneVertexFloats;
            float cz = 0;
            for (int a = 0; a < 3; a++) {
                float centroid = (v[a] + v[kSceneVertexFloats + a] + v[2 * kSceneVertexFloats + a]) / 3.0f;
                cz += cam.m[a * 3 + 2] * (centroid - cam.pos[a]);
            }
            density[face] = std::max(density[face], cam.f / std::max(cz, 0.5f));
        }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);
    glDeleteRenderbuffers(1, &depth);
    glDeleteTextures(1, &ids);
    return density;
}

void projection_attach_uv(ProjectionGpu& gpu, const BakeAtlas& atlas) {
    glBindVertexArray(gpu.vao);
    glDeleteBuffers(1, &gpu.uvVbo);
    glGenBuffers(1, &gpu.uvVbo);
    glBindBuffer(GL_ARRAY_BUFFER, gpu.uvVbo);
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(atlas.uvPage.size() * 4), atlas.uvPage.data(), GL_STATIC_DRAW);
    glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, 12, nullptr);
    glEnableVertexAttribArray(3);
}

void projection_bake(const ProjectionGpu& gpu, const BakeAtlas& atlas, std::vector<std::vector<uint32_t>>& pages) {
    initPrograms();
    int size = atlas.pageSize;
    size_t texels = size_t(size) * size_t(size);
    GLuint targets[3] = {0, 0, 0}, fbo = 0;
    const GLenum formats[3] = {GL_RGBA8, GL_RGBA32F, GL_RGBA8};
    glGenTextures(3, targets);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    for (int t = 0; t < 3; t++) {
        glBindTexture(GL_TEXTURE_2D, targets[t]);
        glTexImage2D(GL_TEXTURE_2D, 0, GLint(formats[t]), size, size, 0, GL_RGBA, t == 1 ? GL_FLOAT : GL_UNSIGNED_BYTE, nullptr);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GLenum(GL_COLOR_ATTACHMENT0 + t), GL_TEXTURE_2D, targets[t], 0);
    }
    const GLenum buffers[3] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2};
    glDrawBuffers(3, buffers);
    glViewport(0, 0, size, size);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glBindVertexArray(gpu.vao);
    auto drawPage = [&](GLuint program, int page) {
        glUniform1f(glGetUniformLocation(program, "page"), float(page));
        glScissor(0, 0, size, size);
        const float zero[4] = {0, 0, 0, 0};
        for (int t = 0; t < 3; t++) {
            glClearBufferfv(GL_COLOR, t, zero);
        }
        for (int y = 0; y < size; y += kBakeTile) {
            for (int x = 0; x < size; x += kBakeTile) {
                glScissor(x, y, kBakeTile, kBakeTile);
                glDrawArrays(GL_TRIANGLES, 0, gpu.vertexCount);
                glFinish();
            }
        }
    };

    FillGrid grid = makeGrid(gpu);
    glUseProgram(g_bakeProgram);
    projection_bind(gpu, g_bakeProgram, 0);
    glUniform1f(glGetUniformLocation(g_bakeProgram, "pageSize"), float(size));
    pages.assign(size_t(atlas.pages), std::vector<uint32_t>(texels));
    std::vector<float> position(texels * 4);
    std::vector<uint32_t> normal(texels);
    glEnable(GL_SCISSOR_TEST);
    for (int p = 0; p < atlas.pages; p++) {
        drawPage(g_bakeProgram, p);
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        glReadPixels(0, 0, size, size, GL_RGBA, GL_UNSIGNED_BYTE, pages[size_t(p)].data());
        glReadBuffer(GL_COLOR_ATTACHMENT1);
        glReadPixels(0, 0, size, size, GL_RGBA, GL_FLOAT, position.data());
        glReadBuffer(GL_COLOR_ATTACHMENT2);
        glReadPixels(0, 0, size, size, GL_RGBA, GL_UNSIGNED_BYTE, normal.data());
        splat(grid, pages[size_t(p)], position, normal);
    }
    position = {};
    normal = {};

    GLuint gridTex[kGridGrids];
    uploadGrid(grid, gridTex);
    glUseProgram(g_fillProgram);
    const char* names[kGridGrids] = {"gridPX", "gridNX", "gridPY", "gridNY", "gridPZ", "gridNZ", "gridAll"};
    float base[kGridGrids * 3];
    for (int t = 0; t < kGridGrids; t++) {
        glActiveTexture(GLenum(GL_TEXTURE0 + t));
        glBindTexture(GL_TEXTURE_3D, gridTex[t]);
        glUniform1i(glGetUniformLocation(g_fillProgram, names[t]), t);
        for (int k = 0; k < 3; k++) {
            base[t * 3 + k] = grid.base[t][3] > 0 ? float(grid.base[t][k] / grid.base[t][3]) : 0.1f;
        }
    }
    glActiveTexture(GL_TEXTURE0);
    float extent[3] = {grid.voxel * float(grid.dims[0]), grid.voxel * float(grid.dims[1]), grid.voxel * float(grid.dims[2])};
    glUniform3fv(glGetUniformLocation(g_fillProgram, "gridOrigin"), 1, grid.origin);
    glUniform3fv(glGetUniformLocation(g_fillProgram, "gridExtent"), 1, extent);
    glUniform1f(glGetUniformLocation(g_fillProgram, "gridVoxel"), grid.voxel);
    glUniform1i(glGetUniformLocation(g_fillProgram, "gridLevels"), kGridLevels);
    glUniform3fv(glGetUniformLocation(g_fillProgram, "gridBase"), kGridGrids, base);
    glUniform1f(glGetUniformLocation(g_fillProgram, "pageSize"), float(size));
    glDrawBuffers(1, buffers);
    std::vector<uint32_t> fill(texels);
    for (int p = 0; p < atlas.pages; p++) {
        drawPage(g_fillProgram, p);
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        glReadPixels(0, 0, size, size, GL_RGBA, GL_UNSIGNED_BYTE, fill.data());
        std::vector<uint32_t>& page = pages[size_t(p)];
        for (size_t i = 0; i < texels; i++) {
            if ((page[i] >> 24) == 1) {
                page[i] = (fill[i] & 0xffffffu) | 0x80000000u;
            }
        }
    }
    glDisable(GL_SCISSOR_TEST);
    glDeleteTextures(kGridGrids, gridTex);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(3, targets);
    std::fprintf(stderr, "[bake] fill grid %dx%dx%d at %.1f units\n", grid.dims[0], grid.dims[1], grid.dims[2], grid.voxel);
}
