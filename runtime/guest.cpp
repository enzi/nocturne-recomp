#include "guest.h"

#include <algorithm>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

uint8_t* g_mem = nullptr;

namespace {

constexpr uint64_t kReserve = 0x100000000ull + 0x10000;
constexpr uint32_t kArenaLo = 0x10000000u;
constexpr uint32_t kArenaHi = 0x7fff0000u;

std::mutex g_allocMutex;
std::map<uint32_t, uint32_t> g_blocks;

#ifdef _WIN32
LONG CALLBACK onFault(EXCEPTION_POINTERS* e) {
    auto addr = reinterpret_cast<uint8_t*>(e->ExceptionRecord->ExceptionInformation[1]);
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || !g_mem || addr < g_mem || addr >= g_mem + kReserve) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (addr >= g_mem + 0x10000 && VirtualAlloc(addr, 1, MEM_COMMIT, PAGE_READWRITE)) {
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    std::fprintf(stderr, "guest memory fault at 0x%08llx\n", static_cast<unsigned long long>(addr - g_mem));
    std::_Exit(139);
}
#else
void onFault(int sig, siginfo_t* info, void*) {
    auto addr = reinterpret_cast<uint8_t*>(info->si_addr);
    char buf[160];
    int n;
    if (g_mem && addr >= g_mem && addr < g_mem + kReserve) {
        n = std::snprintf(buf, sizeof buf, "guest memory fault at 0x%08llx (signal %d)\n", static_cast<unsigned long long>(addr - g_mem), sig);
    } else {
        n = std::snprintf(buf, sizeof buf, "host fault at %p (signal %d)\n", info->si_addr, sig);
    }
    ssize_t ignored = write(2, buf, n);
    (void)ignored;
    std::_Exit(128 + sig);
}
#endif

uint32_t alignUp(uint64_t v, uint32_t a) {
    return static_cast<uint32_t>((v + a - 1) & ~uint64_t(a - 1));
}

bool overlaps(uint32_t addr, uint32_t size) {
    auto it = g_blocks.upper_bound(addr);
    if (it != g_blocks.end() && it->first < uint64_t(addr) + size) {
        return true;
    }
    if (it != g_blocks.begin()) {
        --it;
        if (uint64_t(it->first) + it->second > addr) {
            return true;
        }
    }
    return false;
}

}

void guest_memory_init() {
#ifdef _WIN32
    g_mem = static_cast<uint8_t*>(VirtualAlloc(nullptr, kReserve, MEM_RESERVE, PAGE_NOACCESS));
    if (!g_mem) {
        std::fprintf(stderr, "cannot reserve guest memory (error %lu)\n", GetLastError());
        std::exit(1);
    }
    AddVectoredExceptionHandler(1, onFault);
#else
    void* p = mmap(nullptr, kReserve, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) {
        std::perror("mmap guest memory");
        std::exit(1);
    }
    g_mem = static_cast<uint8_t*>(p);
    mprotect(g_mem, 0x10000, PROT_NONE);
    struct sigaction sa {};
    sa.sa_sigaction = onFault;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);
#endif
}

uint32_t guest_alloc(uint32_t size, uint32_t align) {
    if (size == 0) {
        size = 1;
    }
    std::lock_guard lock(g_allocMutex);
    uint64_t cur = kArenaLo;
    for (auto& [base, len] : g_blocks) {
        uint64_t start = alignUp(cur, align);
        if (start + size <= base) {
            break;
        }
        cur = std::max<uint64_t>(cur, uint64_t(base) + len);
    }
    uint64_t start = alignUp(cur, align);
    if (start + size > kArenaHi) {
        return 0;
    }
    g_blocks[static_cast<uint32_t>(start)] = size;
    std::memset(g_mem + start, 0, size);
    return static_cast<uint32_t>(start);
}

uint32_t guest_alloc_at(uint32_t addr, uint32_t size) {
    std::lock_guard lock(g_allocMutex);
    if (addr < 0x10000 || uint64_t(addr) + size > kArenaHi || overlaps(addr, size)) {
        return 0;
    }
    g_blocks[addr] = size;
    std::memset(g_mem + addr, 0, size);
    return addr;
}

bool guest_free(uint32_t addr) {
    std::lock_guard lock(g_allocMutex);
    auto it = g_blocks.find(addr);
    if (it == g_blocks.end()) {
        return false;
    }
    uint64_t lo = alignUp(it->first, 4096);
    uint64_t hi = (uint64_t(it->first) + it->second) & ~uint64_t(4095);
    if (hi > lo) {
#ifdef _WIN32
        VirtualFree(g_mem + lo, hi - lo, MEM_DECOMMIT);
#else
        madvise(g_mem + lo, hi - lo, MADV_DONTNEED);
#endif
    }
    g_blocks.erase(it);
    return true;
}

bool guest_region(uint32_t addr, uint32_t& base, uint32_t& size) {
    std::lock_guard lock(g_allocMutex);
    auto it = g_blocks.upper_bound(addr);
    if (it == g_blocks.begin()) {
        return false;
    }
    --it;
    if (uint64_t(it->first) + it->second <= addr) {
        return false;
    }
    base = it->first;
    size = it->second;
    return true;
}

void guest_dump(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        std::perror(path.c_str());
        return;
    }
    auto block = [&](uint32_t addr, uint32_t size) {
        std::fwrite(&addr, 4, 1, f);
        std::fwrite(&size, 4, 1, f);
        std::fwrite(g_mem + addr, 1, size, f);
    };
    block(0x400000, 0x02e26000 - 0x400000);
    std::lock_guard lock(g_allocMutex);
    for (auto& [addr, size] : g_blocks) {
        block(addr, size);
    }
    std::fclose(f);
}

std::string gstring(uint32_t a) {
    if (!a) {
        return {};
    }
    return std::string(gstr(a));
}

uint32_t guest_strdup(const std::string& s) {
    uint32_t a = guest_alloc(static_cast<uint32_t>(s.size() + 1), 4);
    std::memcpy(g_mem + a, s.c_str(), s.size() + 1);
    return a;
}

uint32_t copy_string_out(uint32_t dst, uint32_t cap, const std::string& s) {
    if (!dst || cap == 0) {
        return 0;
    }
    uint32_t n = static_cast<uint32_t>(std::min<size_t>(s.size(), cap - 1));
    std::memcpy(g_mem + dst, s.data(), n);
    g_mem[dst + n] = 0;
    return n;
}
