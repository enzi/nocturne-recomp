#include "firstrun.h"

#include "gamesetup.h"

#include <SDL3/SDL.h>

#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct PickedFiles {
    std::mutex mu;
    bool done = false;
    bool failed = false;
    std::vector<fs::path> files;
};

void onFilesPicked(void* userdata, const char* const* list, int) {
    auto* picked = static_cast<PickedFiles*>(userdata);
    std::lock_guard lock(picked->mu);
    if (!list) {
        picked->failed = true;
    } else {
        for (; *list; list++) {
            picked->files.emplace_back(reinterpret_cast<const char8_t*>(*list));
        }
    }
    picked->done = true;
}

void pumpEvents() {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
    }
}

bool askForDiscs(const std::string& message) {
    const SDL_MessageBoxButtonData buttons[] = {
        {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Select discs"},
        {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Quit"},
    };
    SDL_MessageBoxData data{SDL_MESSAGEBOX_INFORMATION, nullptr, "Nocturne setup", message.c_str(), 2, buttons, nullptr};
    int button = 0;
    if (!SDL_ShowMessageBox(&data, &button)) {
        std::fprintf(stderr, "[setup] cannot show a message box: %s\n", SDL_GetError());
        return false;
    }
    return button == 1;
}

bool pickImages(std::vector<fs::path>& images) {
    PickedFiles picked;
    static const SDL_DialogFileFilter filter{"Disc images", "iso"};
    SDL_ShowOpenFileDialog(onFilesPicked, &picked, nullptr, &filter, 1, nullptr, true);
    while (true) {
        pumpEvents();
        {
            std::lock_guard lock(picked.mu);
            if (picked.done) {
                break;
            }
        }
        SDL_Delay(20);
    }
    if (picked.failed) {
        std::string message = std::string("No file dialog is available: ") + SDL_GetError() + "\n\nRun nocsetup to extract the game files instead.";
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Nocturne setup", message.c_str(), nullptr);
        return false;
    }
    images = picked.files;
    return true;
}

bool extractWithProgress(const std::vector<fs::path>& images, const fs::path& gameDir, std::string& error) {
    SDL_Window* window = SDL_CreateWindow("Nocturne setup", 520, 80, 0);
    SDL_Renderer* renderer = window ? SDL_CreateRenderer(window, nullptr) : nullptr;
    std::mutex mu;
    std::string file;
    uint64_t done = 0;
    uint64_t total = 1;
    bool finished = false;
    bool ok = false;
    std::thread worker([&] {
        std::string message;
        bool result = setup_extract(images, gameDir, [&](uint64_t d, uint64_t t, const std::string& f) {
            std::lock_guard lock(mu);
            done = d;
            total = t ? t : 1;
            file = f;
        }, message);
        std::lock_guard lock(mu);
        ok = result;
        error = message;
        finished = true;
    });
    while (true) {
        pumpEvents();
        std::string label;
        float fraction;
        {
            std::lock_guard lock(mu);
            if (finished) {
                break;
            }
            label = "Copying " + file;
            fraction = float(double(done) / double(total));
        }
        if (renderer) {
            SDL_SetRenderDrawColor(renderer, 16, 16, 16, 255);
            SDL_RenderClear(renderer);
            SDL_SetRenderDrawColor(renderer, 200, 200, 200, 255);
            SDL_RenderDebugText(renderer, 16, 16, label.c_str());
            SDL_FRect border{16, 40, 488, 20};
            SDL_RenderRect(renderer, &border);
            SDL_FRect bar{18, 42, 484 * fraction, 16};
            SDL_RenderFillRect(renderer, &bar);
            SDL_RenderPresent(renderer);
        }
        SDL_Delay(30);
    }
    worker.join();
    if (renderer) {
        SDL_DestroyRenderer(renderer);
    }
    if (window) {
        SDL_DestroyWindow(window);
    }
    return ok;
}

}

bool firstrun_setup(const fs::path& gameDir) {
    if (setup_installed(gameDir)) {
        return true;
    }
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "[setup] the game files are missing in %s, and no window can open to ask for the discs: %s\n", gameDir.string().c_str(), SDL_GetError());
        return false;
    }
    std::string message = "The Nocturne game files are not installed yet.\n\nSelect the ISO images of both Nocturne discs. The game copies its files from them into\n" + gameDir.string();
    bool installed = false;
    while (askForDiscs(message)) {
        std::vector<fs::path> images;
        if (!pickImages(images)) {
            break;
        }
        if (images.empty()) {
            continue;
        }
        std::string error;
        if (extractWithProgress(images, gameDir, error)) {
            installed = true;
            break;
        }
        std::fprintf(stderr, "[setup] %s\n", error.c_str());
        message = "Setup failed: " + error + "\n\nSelect the ISO images of both Nocturne discs to try again.";
    }
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    return installed;
}
