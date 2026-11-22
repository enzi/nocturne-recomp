#include "firstrun.h"
#include "options.h"
#include "pe.h"
#include "win32.h"
#include "world3d.h"

#include <SDL3/SDL.h>

#include <cstdio>

namespace fs = std::filesystem;

namespace {

fs::path defaultGameDir() {
    const char* base = SDL_GetBasePath();
    return (base ? fs::path(reinterpret_cast<const char8_t*>(base)) : fs::current_path()) / "game";
}

}

int main(int argc, char** argv) {
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if (argc > 2) {
        std::fprintf(stderr, "usage: %s [game folder]\n", argv[0]);
        return 2;
    }
    fs::path gameDir = argc == 2 ? fs::absolute(argv[1]) : defaultGameDir();
#ifdef _WIN32
    std::error_code ec;
    fs::create_directories(gameDir, ec);
    if (_wfreopen((gameDir / "nocturne.log").c_str(), L"w", stderr)) {
        std::setvbuf(stderr, nullptr, _IONBF, 0);
    }
#endif
    if (!firstrun_setup(gameDir)) {
        return 1;
    }
    PeImage pe;
    std::string err;
    if (!pe.load((gameDir / "nocturne.exe").string(), err)) {
        std::fprintf(stderr, "%s: %s\n", (gameDir / "nocturne.exe").string().c_str(), err.c_str());
        return 1;
    }

    guest_memory_init();
    std::memcpy(g_mem + pe.imageBase, pe.file.data(), std::min<size_t>(pe.headerSize, pe.file.size()));
    for (const PeSection& s : pe.sections) {
        if (s.rawSize) {
            std::memcpy(g_mem + s.va, pe.file.data() + s.rawPtr, s.rawSize);
        }
    }
    for (const PeImport& imp : pe.imports) {
        std::string name = imp.name.empty() ? "#" + std::to_string(imp.ordinal) : imp.name;
        wr32(g_mem, imp.iatSlot, hle_thunk(imp.dll, name));
    }

    win32_init(gameDir);
    options_load(gameDir);
    world3d_init(gameDir);
    kernel32_init("\"" + guest_exe_path() + "\"");

    Ctx c;
    thread_setup(c, 0x100000);
    push32(&c, g_mem, 0);
    try {
        call_indirect(&c, g_mem, pe.entry);
    } catch (const ThreadExit& e) {
        process_exit(e.code);
    }
    std::fprintf(stderr, "entry point returned 0x%08x\n", c.eax);
    process_exit(c.eax);
}
