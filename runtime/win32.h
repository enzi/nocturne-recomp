#pragma once

#include "hle.h"

#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

constexpr uint32_t kInvalidHandle = 0xffffffffu;
constexpr uint32_t kImageBase = 0x00400000u;

constexpr uint32_t ERROR_FILE_NOT_FOUND = 2;
constexpr uint32_t ERROR_PATH_NOT_FOUND = 3;
constexpr uint32_t ERROR_ACCESS_DENIED = 5;
constexpr uint32_t ERROR_INVALID_HANDLE = 6;
constexpr uint32_t ERROR_NOT_ENOUGH_MEMORY = 8;
constexpr uint32_t ERROR_NO_MORE_FILES = 18;
constexpr uint32_t ERROR_FILE_EXISTS = 80;
constexpr uint32_t ERROR_INVALID_PARAMETER = 87;
constexpr uint32_t ERROR_ALREADY_EXISTS = 183;

struct KObject {
    virtual ~KObject() = default;
};

struct KFile : KObject {
    int fd = -1;
    bool console = false;
    ~KFile() override;
};

struct KWaitable : KObject {
    std::mutex mu;
    std::condition_variable cv;
    virtual bool tryAcquire() = 0;
    virtual void acquired() {}
};

struct KEvent : KWaitable {
    bool manual = false;
    bool signaled = false;
    bool tryAcquire() override;
};

struct KMutex : KWaitable {
    std::thread::id owner;
    uint32_t count = 0;
    bool tryAcquire() override;
};

struct KThread : KWaitable {
    bool done = false;
    uint32_t exitCode = 0;
    bool tryAcquire() override { return done; }
};

uint32_t handle_add(std::shared_ptr<KObject> obj);
std::shared_ptr<KObject> handle_get(uint32_t h);
bool handle_close(uint32_t h);

template <typename T>
std::shared_ptr<T> handle_as(uint32_t h) {
    return std::dynamic_pointer_cast<T>(handle_get(h));
}

void win32_init(const std::filesystem::path& hostGameDir);
void kernel32_init(const std::string& commandLine);
std::string guest_full_path(const std::string& path);
std::filesystem::path host_path(const std::string& guestPath);
std::string guest_cwd();
void set_guest_cwd(const std::string& path);
std::string guest_exe_path();

void set_last_error(Ctx* c, uint32_t err);
uint32_t last_error(Ctx* c);

uint32_t thread_setup(Ctx& c, uint32_t stackSize);
[[noreturn]] void process_exit(uint32_t code);

struct ThreadExit {
    uint32_t code;
};
