#include "display.h"

#include "gl.h"

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

SDL_Window* user32_window();

namespace {

SDL_GLContext g_context = nullptr;
GLuint g_program = 0;
GLuint g_vao = 0;
GLuint g_uploadTexture = 0;
GLint g_rectLocation = -1;
int g_uploadWidth = 0;
int g_uploadHeight = 0;

const char* kVertexShader = R"(#version 330 core
uniform vec4 rect;
out vec2 uv;
void main() {
    vec2 p = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));
    uv = vec2(p.x, 1.0 - p.y);
    gl_Position = vec4(mix(rect.xy, rect.zw, p), 0.0, 1.0);
}
)";

const char* kFragmentShader = R"(#version 330 core
uniform sampler2D image;
in vec2 uv;
out vec4 color;
void main() {
    color = vec4(texture(image, uv).rgb, 1.0);
}
)";

GLuint compile(GLenum type, const char* source) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &source, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(s, sizeof log, nullptr, log);
        std::fprintf(stderr, "[display] shader compile failed: %s\n", log);
    }
    return s;
}

void saveScreenshot(GLuint texture, int w, int h) {
    static const char* path = std::getenv("NOC_SCREENSHOT");
    static unsigned index = 0;
    static uint64_t last = 0;
    static const uint64_t interval = std::getenv("NOC_SCREENSHOT_MS") ? std::strtoull(std::getenv("NOC_SCREENSHOT_MS"), nullptr, 10) : 500;
    if (!path || SDL_GetTicks() - last < interval) {
        return;
    }
    last = SDL_GetTicks();
    std::vector<uint32_t> pixels(size_t(w) * h);
    glBindTexture(GL_TEXTURE_2D, texture);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, pixels.data());
    std::string file = path;
    if (!file.empty() && file.back() == '/') {
        char name[32];
        std::snprintf(name, sizeof name, "shot_%05u.bmp", index++);
        file += name;
    }
    if (SDL_Surface* shot = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_XRGB8888, pixels.data(), w * 4)) {
        SDL_SaveBMP(shot, file.c_str());
        SDL_DestroySurface(shot);
    }
}

}

void display_prepare_window_attributes() {
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
}

bool display_init() {
    if (g_context) {
        return true;
    }
    SDL_Window* win = user32_window();
    if (!win) {
        return false;
    }
    g_context = SDL_GL_CreateContext(win);
    if (!g_context) {
        std::fprintf(stderr, "[display] SDL_GL_CreateContext failed: %s\n", SDL_GetError());
        return false;
    }
    if (!gl_load()) {
        return false;
    }
    SDL_GL_SetSwapInterval(1);
    std::fprintf(stderr, "[display] OpenGL %s on %s\n", reinterpret_cast<const char*>(glGetString(GL_VERSION)),
                 reinterpret_cast<const char*>(glGetString(GL_RENDERER)));
    g_program = glCreateProgram();
    glAttachShader(g_program, compile(GL_VERTEX_SHADER, kVertexShader));
    glAttachShader(g_program, compile(GL_FRAGMENT_SHADER, kFragmentShader));
    glLinkProgram(g_program);
    g_rectLocation = glGetUniformLocation(g_program, "rect");
    glGenVertexArrays(1, &g_vao);
    glGenTextures(1, &g_uploadTexture);
    glBindTexture(GL_TEXTURE_2D, g_uploadTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return true;
}

void display_drawable_size(int& w, int& h) {
    w = 640;
    h = 480;
    if (SDL_Window* win = user32_window()) {
        SDL_GetWindowSizeInPixels(win, &w, &h);
    }
}

void display_present_texture(uint32_t texture, int w, int h) {
    int dw, dh;
    display_drawable_size(dw, dh);
    float sx = 1.0f;
    float sy = 1.0f;
    if (float(dw) * h > float(dh) * w) {
        sx = float(dh) * w / (float(dw) * h);
    } else {
        sy = float(dw) * h / (float(dh) * w);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, dw, dh);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(g_program);
    glUniform4f(g_rectLocation, -sx, -sy, sx, sy);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);
    glBindVertexArray(g_vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    saveScreenshot(texture, w, h);
    SDL_GL_SwapWindow(user32_window());
    static const bool fps = std::getenv("NOC_FPS") != nullptr;
    static uint64_t start = SDL_GetTicks();
    static unsigned frames = 0;
    frames++;
    if (fps && SDL_GetTicks() - start >= 5000) {
        std::fprintf(stderr, "[display] %.1f fps at %dx%d\n", frames * 1000.0 / double(SDL_GetTicks() - start), w, h);
        start = SDL_GetTicks();
        frames = 0;
    }
}

void display_present_bgrx(const void* pixels, int w, int h, int pitchBytes) {
    if (!display_init()) {
        return;
    }
    glBindTexture(GL_TEXTURE_2D, g_uploadTexture);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, pitchBytes / 4);
    if (w != g_uploadWidth || h != g_uploadHeight) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, pixels);
        g_uploadWidth = w;
        g_uploadHeight = h;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, pixels);
    }
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    display_present_texture(g_uploadTexture, w, h);
}
