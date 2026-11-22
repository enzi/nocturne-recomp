#pragma once

#include "bake.h"
#include "scene.h"

#include "gl.h"

#include <vector>

struct ProjectionGpu {
    GLuint vao = 0;
    GLuint vbo = 0;
    GLuint uvVbo = 0;
    GLuint backdrops = 0;
    GLuint depthMaps = 0;
    GLuint cameraBuffer = 0;
    GLuint cameraTex = 0;
    GLsizei vertexCount = 0;
    GLsizei cameraCount = 0;
    float boundsMin[3] = {0, 0, 0};
    float boundsMax[3] = {0, 0, 0};
};

extern const char* const kProjectionGlsl;

GLuint projection_link(const char* vs, const char* fs);
GLuint projection_depth_program();
bool projection_create(const Scene& scene, ProjectionGpu& gpu);
void projection_destroy(ProjectionGpu& gpu);
void projection_bind(const ProjectionGpu& gpu, GLuint program, int firstUnit);
std::vector<float> projection_face_density(const ProjectionGpu& gpu, const Scene& scene);
void projection_attach_uv(ProjectionGpu& gpu, const BakeAtlas& atlas);
void projection_bake(const ProjectionGpu& gpu, const BakeAtlas& atlas, std::vector<std::vector<uint32_t>>& pages);
