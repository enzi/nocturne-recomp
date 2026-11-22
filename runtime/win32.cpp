#include "win32.h"

#include <atomic>
#include <cctype>
#include <cstdio>
#include <map>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

std::mutex g_handleMutex;
std::map<uint32_t, std::shared_ptr<KObject>> g_handles;
uint32_t g_nextHandle = 0x100;

std::mutex g_cwdMutex;
std::string g_cwd = "C:\\NOCTURNE";
fs::path g_hostRoot;
uint32_t g_peb = 0;
std::atomic<uint32_t> g_nextThreadId{0x100};

bool iequals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); i++) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

std::vector<std::string> split(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char ch : s) {
        if (ch == '\\') {
            if (!cur.empty()) {
                out.push_back(cur);
            }
            cur.clear();
        } else {
            cur += ch;
        }
    }
    if (!cur.empty()) {
        out.push_back(cur);
    }
    return out;
}

fs::path resolveComponent(const fs::path& dir, const std::string& name) {
    fs::path p = dir / name;
    std::error_code ec;
    if (fs::exists(fs::symlink_status(p, ec))) {
        return p;
    }
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (iequals(e.path().filename().string(), name)) {
            return e.path();
        }
    }
    return p;
}

}

KFile::~KFile() {
    if (fd >= 0 && !console) {
        close(fd);
    }
}

bool KEvent::tryAcquire() {
    if (!signaled) {
        return false;
    }
    if (!manual) {
        signaled = false;
    }
    return true;
}

bool KMutex::tryAcquire() {
    if (count == 0 || owner == std::this_thread::get_id()) {
        owner = std::this_thread::get_id();
        count++;
        return true;
    }
    return false;
}

uint32_t handle_add(std::shared_ptr<KObject> obj) {
    std::lock_guard lock(g_handleMutex);
    uint32_t h = g_nextHandle;
    g_nextHandle += 4;
    g_handles[h] = std::move(obj);
    return h;
}

std::shared_ptr<KObject> handle_get(uint32_t h) {
    std::lock_guard lock(g_handleMutex);
    auto it = g_handles.find(h);
    return it == g_handles.end() ? nullptr : it->second;
}

bool handle_close(uint32_t h) {
    std::lock_guard lock(g_handleMutex);
    return g_handles.erase(h) != 0;
}

void win32_init(const fs::path& hostGameDir) {
    g_hostRoot = hostGameDir;
    g_peb = guest_alloc(0x1000, 0x1000);
    wr32(g_mem, g_peb + 0x08, kImageBase);
    for (int fd = 0; fd < 3; fd++) {
        auto f = std::make_shared<KFile>();
        f->fd = fd;
        f->console = true;
        std::lock_guard lock(g_handleMutex);
        g_handles[0x10 + 4 * fd] = f;
    }
}

std::string guest_full_path(const std::string& path) {
    std::string s = path;
    for (char& ch : s) {
        if (ch == '/') {
            ch = '\\';
        }
    }
    std::string full;
    if (s.size() >= 2 && s[1] == ':') {
        full = s;
    } else if (!s.empty() && s[0] == '\\') {
        full = "C:" + s;
    } else {
        full = guest_cwd() + "\\" + s;
    }
    std::vector<std::string> parts = split(full);
    std::vector<std::string> out;
    for (size_t i = 0; i < parts.size(); i++) {
        if (i == 0) {
            std::string drive = parts[0].substr(0, 2);
            drive[0] = char(std::toupper(static_cast<unsigned char>(drive[0])));
            out.push_back(drive);
            if (parts[0].size() > 2) {
                out.push_back(parts[0].substr(2));
            }
        } else if (parts[i] == ".") {
            continue;
        } else if (parts[i] == "..") {
            if (out.size() > 1) {
                out.pop_back();
            }
        } else {
            out.push_back(parts[i]);
        }
    }
    std::string r = out.empty() ? "C:" : out[0];
    if (out.size() == 1) {
        return r + "\\";
    }
    for (size_t i = 1; i < out.size(); i++) {
        r += "\\" + out[i];
    }
    return r;
}

fs::path host_path(const std::string& guestPath) {
    std::vector<std::string> parts = split(guest_full_path(guestPath));
    fs::path p;
    size_t i = 1;
    if (parts.size() >= 2 && iequals(parts[1], "NOCTURNE")) {
        p = g_hostRoot;
        i = 2;
    } else {
        p = g_hostRoot / ".drive_c";
    }
    for (; i < parts.size(); i++) {
        p = resolveComponent(p, parts[i]);
    }
    return p;
}

std::string guest_cwd() {
    std::lock_guard lock(g_cwdMutex);
    return g_cwd;
}

void set_guest_cwd(const std::string& path) {
    std::string full = guest_full_path(path);
    std::lock_guard lock(g_cwdMutex);
    g_cwd = full;
}

std::string guest_exe_path() {
    return "C:\\NOCTURNE\\nocturne.exe";
}

void set_last_error(Ctx* c, uint32_t err) {
    wr32(g_mem, c->fs_base + 0x34, err);
}

uint32_t last_error(Ctx* c) {
    return rd32(g_mem, c->fs_base + 0x34);
}

uint32_t thread_setup(Ctx& c, uint32_t stackSize) {
    if (stackSize < 0x100000) {
        stackSize = 0x100000;
    }
    uint32_t stack = guest_alloc(stackSize, 0x10000);
    uint32_t teb = guest_alloc(0x1000, 0x1000);
    if (!stack || !teb) {
        std::fprintf(stderr, "out of guest memory for a thread\n");
        std::abort();
    }
    uint32_t top = stack + stackSize;
    wr32(g_mem, teb + 0x00, 0xffffffffu);
    wr32(g_mem, teb + 0x04, top);
    wr32(g_mem, teb + 0x08, stack);
    wr32(g_mem, teb + 0x18, teb);
    wr32(g_mem, teb + 0x20, static_cast<uint32_t>(getpid()));
    wr32(g_mem, teb + 0x24, g_nextThreadId.fetch_add(4));
    wr32(g_mem, teb + 0x2c, teb + 0xe10);
    wr32(g_mem, teb + 0x30, g_peb);
    std::memset(&c, 0, sizeof c);
    fpu_init(&c);
    c.fcw = 0x27f;
    c.esp = top - 16;
    c.fs_base = teb;
    return teb;
}

void process_exit(uint32_t code) {
    std::fflush(stdout);
    std::fflush(stderr);
    std::_Exit(static_cast<int>(code));
}
