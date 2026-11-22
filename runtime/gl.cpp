#include "gl.h"

#include <SDL3/SDL.h>

#include <cstdio>

#ifdef _WIN32
#define X(type, name) type name = nullptr;
GL_FUNCTIONS(X)
#undef X
#endif

bool gl_load() {
    bool ok = true;
#ifdef _WIN32
#define X(type, name)                                                                     \
    name = reinterpret_cast<type>(SDL_GL_GetProcAddress(#name));                          \
    if (!name) {                                                                          \
        std::fprintf(stderr, "OpenGL function %s is missing\n", #name);                    \
        ok = false;                                                                       \
    }
    GL_FUNCTIONS(X)
#undef X
#endif
    return ok;
}
