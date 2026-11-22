#include "win32.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fcntl.h>
#include <map>
#include <sys/stat.h>
#include <unistd.h>

#ifdef _WIN32
#include <sys/utime.h>
#endif

namespace fs = std::filesystem;

namespace {

uint32_t g_cmdLineA = 0;
uint32_t g_cmdLineW = 0;
uint32_t g_envBlock = 0;
uint32_t g_unhandledFilter = 0;

std::mutex g_csMutex;
std::map<uint32_t, std::shared_ptr<std::recursive_mutex>> g_criticalSections;

std::mutex g_tlsMutex;
uint64_t g_tlsUsed = 0;

std::mutex g_moduleMutex;
std::map<uint32_t, std::string> g_modules;

struct KFind : KObject {
    std::vector<fs::path> entries;
    size_t next = 0;
};

constexpr uint64_t kFiletimeUnixOffset = 116444736000000000ull;

int64_t daysFromCivil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return int64_t(era) * 146097 + int64_t(doe) - 719468;
}

void civilFromDays(int64_t z, int& y, unsigned& m, unsigned& d) {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    y = static_cast<int>(int64_t(yoe) + era * 400) + (m <= 2);
}

uint64_t filetimeFromUnix(int64_t sec, int64_t nsec) {
    return uint64_t(sec) * 10000000ull + uint64_t(nsec / 100) + kFiletimeUnixOffset;
}

uint64_t readFiletime(uint32_t p) {
    return rd64(g_mem, p);
}

void writeFiletime(uint32_t p, uint64_t ft) {
    if (p) {
        wr64(g_mem, p, ft);
    }
}

void writeSystemtime(uint32_t p, uint64_t ft) {
    int64_t total100ns = int64_t(ft - kFiletimeUnixOffset);
    int64_t secs = total100ns / 10000000;
    int64_t ms = (total100ns % 10000000) / 10000;
    int64_t days = secs / 86400;
    int64_t rem = secs % 86400;
    if (rem < 0) {
        rem += 86400;
        days--;
    }
    int y;
    unsigned mo, d;
    civilFromDays(days, y, mo, d);
    uint16_t st[8] = {uint16_t(y), uint16_t(mo), uint16_t(((days % 7) + 11) % 7), uint16_t(d),
                      uint16_t(rem / 3600), uint16_t(rem / 60 % 60), uint16_t(rem % 60), uint16_t(ms)};
    std::memcpy(g_mem + p, st, sizeof st);
}

uint64_t readSystemtime(uint32_t p) {
    uint16_t st[8];
    std::memcpy(st, g_mem + p, sizeof st);
    int64_t days = daysFromCivil(st[0], st[1], st[3]);
    int64_t secs = days * 86400 + st[4] * 3600 + st[5] * 60 + st[6];
    return filetimeFromUnix(secs, int64_t(st[7]) * 1000000);
}

std::shared_ptr<std::recursive_mutex> criticalSection(uint32_t addr) {
    std::lock_guard lock(g_csMutex);
    auto& cs = g_criticalSections[addr];
    if (!cs) {
        cs = std::make_shared<std::recursive_mutex>();
    }
    return cs;
}

uint32_t win32Error(const std::error_code& ec) {
    if (ec == std::errc::no_such_file_or_directory) {
        return ERROR_FILE_NOT_FOUND;
    }
    if (ec == std::errc::not_a_directory) {
        return ERROR_PATH_NOT_FOUND;
    }
    if (ec == std::errc::file_exists) {
        return ERROR_FILE_EXISTS;
    }
    return ERROR_ACCESS_DENIED;
}

uint32_t errnoToWin32(int e) {
    return win32Error(std::error_code(e, std::generic_category()));
}

uint64_t pathFiletime(const fs::path& path) {
    std::error_code ec;
    auto t = fs::last_write_time(path, ec);
    if (ec) {
        return kFiletimeUnixOffset;
    }
    int64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::file_clock::to_sys(t).time_since_epoch()).count();
    return filetimeFromUnix(ns / 1000000000, ns % 1000000000);
}

uint64_t statFiletime(const struct stat& st) {
#ifdef _WIN32
    return filetimeFromUnix(st.st_mtime, 0);
#else
    return filetimeFromUnix(st.st_mtim.tv_sec, st.st_mtim.tv_nsec);
#endif
}

int openHost(const fs::path& path, int flags) {
#ifdef _WIN32
    return _wopen(path.c_str(), flags | O_BINARY, _S_IREAD | _S_IWRITE);
#else
    return open(path.c_str(), flags | O_CLOEXEC, 0644);
#endif
}

bool wildcardMatch(const char* mask, const char* name) {
    if (*mask == '*') {
        return wildcardMatch(mask + 1, name) || (*name && wildcardMatch(mask, name + 1));
    }
    if (!*mask) {
        return !*name;
    }
    return *name && (*mask == '?' || std::tolower(static_cast<unsigned char>(*mask)) == std::tolower(static_cast<unsigned char>(*name))) && wildcardMatch(mask + 1, name + 1);
}

void fillFindData(uint32_t p, const fs::path& path) {
    std::memset(g_mem + p, 0, 320);
    std::error_code ec;
    bool dir = fs::is_directory(path, ec);
    uint64_t size = dir ? 0 : fs::file_size(path, ec);
    if (ec) {
        size = 0;
    }
    wr32(g_mem, p, dir ? 0x10 : 0x20);
    uint64_t ft = pathFiletime(path);
    writeFiletime(p + 4, ft);
    writeFiletime(p + 12, ft);
    writeFiletime(p + 20, ft);
    wr32(g_mem, p + 28, uint32_t(size >> 32));
    wr32(g_mem, p + 32, uint32_t(size));
    copy_string_out(p + 44, 260, path.filename().string());
}

uint32_t moduleHandle(const std::string& name) {
    std::string base = name;
    if (auto slash = base.find_last_of("\\/"); slash != std::string::npos) {
        base = base.substr(slash + 1);
    }
    for (char& ch : base) {
        ch = char(std::tolower(static_cast<unsigned char>(ch)));
    }
    if (base.find('.') == std::string::npos) {
        base += ".dll";
    }
    std::lock_guard lock(g_moduleMutex);
    for (auto& [h, n] : g_modules) {
        if (n == base) {
            return h;
        }
    }
    uint32_t h = 0xe0000000u + 0x10000u * static_cast<uint32_t>(g_modules.size());
    g_modules[h] = base;
    return h;
}

std::string moduleName(uint32_t h) {
    std::lock_guard lock(g_moduleMutex);
    auto it = g_modules.find(h);
    return it == g_modules.end() ? std::string() : it->second;
}

bool isLoadableModule(const std::string& name) {
    static const char* known[] = {"kernel32.dll", "user32.dll", "gdi32.dll", "winmm.dll", "advapi32.dll", "ddraw.dll"};
    std::string lower;
    for (char ch : name.substr(name.find_last_of("\\/") == std::string::npos ? 0 : name.find_last_of("\\/") + 1)) {
        lower += char(std::tolower(static_cast<unsigned char>(ch)));
    }
    for (const char* k : known) {
        if (lower == k) {
            return true;
        }
    }
    return false;
}

bool isRendererModule(const std::string& name) {
    std::string lower;
    for (char ch : name.substr(name.find_last_of("\\/") == std::string::npos ? 0 : name.find_last_of("\\/") + 1)) {
        lower += char(std::tolower(static_cast<unsigned char>(ch)));
    }
    return lower == "tridx6.dll" || lower == "tridx7.dll" || lower == "trid3d.dll" || lower == "tri3dfx.dll";
}

}

void kernel32_init(const std::string& commandLine) {
    g_cmdLineA = guest_strdup(commandLine);
    g_cmdLineW = guest_alloc(static_cast<uint32_t>(2 * (commandLine.size() + 1)), 4);
    for (size_t i = 0; i < commandLine.size(); i++) {
        wr16(g_mem, g_cmdLineW + 2 * static_cast<uint32_t>(i), static_cast<uint8_t>(commandLine[i]));
    }
    g_envBlock = guest_alloc(4, 4);
}

HLE("kernel32", GetVersion, 0) { return 0x08930005u; }
HLE("kernel32", GetCommandLineA, 0) { return g_cmdLineA; }
HLE("kernel32", GetCommandLineW, 0) { return g_cmdLineW; }
HLE("kernel32", GetEnvironmentStrings, 0) { return g_envBlock; }
HLE("kernel32", FreeEnvironmentStringsA, 1) { return 1; }
HLE("kernel32", SetEnvironmentVariableA, 2) { return 1; }
HLE("kernel32", SetEnvironmentVariableW, 2) { return 1; }
HLE("kernel32", GetACP, 0) { return 1252; }
HLE("kernel32", GetOEMCP, 0) { return 437; }

HLE("kernel32", GetCPInfo, 2) {
    std::memset(g_mem + a[1], 0, 20);
    wr32(m, a[1], 1);
    m[a[1] + 4] = '?';
    return 1;
}

HLE("kernel32", MultiByteToWideChar, 6) {
    uint32_t src = a[2];
    int32_t srcLen = int32_t(a[3]);
    uint32_t dst = a[4];
    int32_t dstLen = int32_t(a[5]);
    int32_t n = srcLen < 0 ? int32_t(std::strlen(gstr(src)) + 1) : srcLen;
    if (dstLen == 0) {
        return uint32_t(n);
    }
    if (dstLen < n) {
        set_last_error(c, 122);
        return 0;
    }
    for (int32_t i = 0; i < n; i++) {
        wr16(m, dst + 2 * i, m[src + i]);
    }
    return uint32_t(n);
}

HLE("kernel32", WideCharToMultiByte, 8) {
    uint32_t src = a[2];
    int32_t srcLen = int32_t(a[3]);
    uint32_t dst = a[4];
    int32_t dstLen = int32_t(a[5]);
    int32_t n = srcLen;
    if (n < 0) {
        n = 0;
        while (rd16(m, src + 2 * n)) {
            n++;
        }
        n++;
    }
    if (dstLen == 0) {
        return uint32_t(n);
    }
    if (dstLen < n) {
        set_last_error(c, 122);
        return 0;
    }
    for (int32_t i = 0; i < n; i++) {
        uint16_t ch = rd16(m, src + 2 * i);
        m[dst + i] = ch > 0xff ? '?' : uint8_t(ch);
    }
    return uint32_t(n);
}

HLE("kernel32", GetModuleHandleA, 1) {
    if (a[0] == 0) {
        return kImageBase;
    }
    std::string name = gstring(a[0]);
    if (!isLoadableModule(name) && !isLoadableModule(name + ".dll")) {
        set_last_error(c, 126);
        return 0;
    }
    return moduleHandle(name);
}

HLE("kernel32", GetModuleFileNameA, 3) {
    std::string path = (a[0] == 0 || a[0] == kImageBase) ? guest_exe_path() : "C:\\WINDOWS\\SYSTEM\\" + moduleName(a[0]);
    return copy_string_out(a[1], a[2], path);
}

HLE("kernel32", GetModuleFileNameW, 3) {
    std::string path = (a[0] == 0 || a[0] == kImageBase) ? guest_exe_path() : "C:\\WINDOWS\\SYSTEM\\" + moduleName(a[0]);
    uint32_t cap = a[2];
    if (!cap) {
        return 0;
    }
    uint32_t n = std::min<uint32_t>(static_cast<uint32_t>(path.size()), cap - 1);
    for (uint32_t i = 0; i < n; i++) {
        wr16(m, a[1] + 2 * i, static_cast<uint8_t>(path[i]));
    }
    wr16(m, a[1] + 2 * n, 0);
    return n;
}

HLE("kernel32", LoadLibraryA, 1) {
    std::string name = gstring(a[0]);
    if (isLoadableModule(name)) {
        return moduleHandle(name);
    }
    if (isRendererModule(name)) {
        std::fprintf(stderr, "[kernel32] LoadLibraryA(\"%s\") -> native renderer\n", name.c_str());
        return moduleHandle("tridx6.dll");
    }
    std::fprintf(stderr, "[kernel32] LoadLibraryA(\"%s\") refused\n", name.c_str());
    set_last_error(c, 126);
    return 0;
}

HLE("kernel32", FreeLibrary, 1) { return 1; }

HLE("kernel32", GetProcAddress, 2) {
    std::string mod = moduleName(a[0]);
    if (mod.empty() || a[1] < 0x10000) {
        set_last_error(c, 127);
        return 0;
    }
    return hle_thunk(mod, gstring(a[1]));
}

HLE("kernel32", GetCurrentProcess, 0) { return 0xffffffffu; }
HLE("kernel32", GetCurrentThread, 0) { return 0xfffffffeu; }
HLE("kernel32", GetCurrentProcessId, 0) { return rd32(m, c->fs_base + 0x20); }
HLE("kernel32", GetCurrentThreadId, 0) { return rd32(m, c->fs_base + 0x24); }
HLE("kernel32", GetLastError, 0) { return last_error(c); }

HLE("kernel32", SetLastError, 1) {
    set_last_error(c, a[0]);
    return 0;
}

HLE("kernel32", ExitProcess, 1) { process_exit(a[0]); }
HLE("kernel32", ExitThread, 1) { throw ThreadExit{a[0]}; }

HLE("kernel32", SetUnhandledExceptionFilter, 1) {
    uint32_t old = g_unhandledFilter;
    g_unhandledFilter = a[0];
    return old;
}

HLE("kernel32", UnhandledExceptionFilter, 1) { return 1; }

HLE("kernel32", GetStdHandle, 1) {
    switch (a[0]) {
    case 0xfffffff6u: return 0x10;
    case 0xfffffff5u: return 0x14;
    case 0xfffffff4u: return 0x18;
    default:
        set_last_error(c, ERROR_INVALID_HANDLE);
        return kInvalidHandle;
    }
}

HLE("kernel32", SetStdHandle, 2) { return 1; }

HLE("kernel32", GetConsoleMode, 2) {
    set_last_error(c, ERROR_INVALID_HANDLE);
    return 0;
}

HLE("kernel32", SetConsoleMode, 2) {
    set_last_error(c, ERROR_INVALID_HANDLE);
    return 0;
}

HLE("kernel32", ReadConsoleInputA, 4) {
    set_last_error(c, ERROR_INVALID_HANDLE);
    return 0;
}

HLE("kernel32", WriteConsoleA, 5) {
    std::fwrite(g_mem + a[1], 1, a[2], stderr);
    if (a[3]) {
        wr32(m, a[3], a[2]);
    }
    return 1;
}

HLE("kernel32", SetConsoleCtrlHandler, 2) { return 1; }

HLE("kernel32", VirtualAlloc, 4) {
    uint32_t addr = a[0];
    uint32_t size = (a[1] + 0xfff) & ~0xfffu;
    uint32_t type = a[2];
    uint32_t r = 0;
    uint32_t base, len;
    if (addr == 0) {
        r = guest_alloc(size, 0x10000);
    } else if (guest_region(addr, base, len)) {
        r = (type & 0x1000) ? (addr & ~0xfffu) : 0;
    } else {
        uint32_t start = addr & ~0xffffu;
        r = guest_alloc_at(start, size + (addr - start));
        if (r) {
            r = addr & ~0xfffu;
        }
    }
    if (!r) {
        set_last_error(c, ERROR_NOT_ENOUGH_MEMORY);
    }
    return r;
}

HLE("kernel32", VirtualFree, 3) {
    if (a[2] & 0x8000) {
        return guest_free(a[0]) ? 1 : 0;
    }
    if (a[2] & 0x4000) {
        std::memset(g_mem + a[0], 0, a[1]);
    }
    return 1;
}

HLE("kernel32", VirtualQuery, 3) {
    uint32_t addr = a[0];
    uint32_t p = a[1];
    std::memset(g_mem + p, 0, 28);
    uint32_t base, len;
    if (guest_region(addr, base, len)) {
        wr32(m, p, addr & ~0xfffu);
        wr32(m, p + 4, base);
        wr32(m, p + 8, 4);
        wr32(m, p + 12, ((base + len + 0xfff) & ~0xfffu) - (addr & ~0xfffu));
        wr32(m, p + 16, 0x1000);
        wr32(m, p + 20, 4);
        wr32(m, p + 24, 0x20000);
    } else if (addr >= kImageBase && addr < 0x02e26000u) {
        wr32(m, p, addr & ~0xfffu);
        wr32(m, p + 4, kImageBase);
        wr32(m, p + 8, 0x80);
        wr32(m, p + 12, 0x02e26000u - (addr & ~0xfffu));
        wr32(m, p + 16, 0x1000);
        wr32(m, p + 20, 4);
        wr32(m, p + 24, 0x1000000);
    } else {
        wr32(m, p, addr & ~0xfffu);
        wr32(m, p + 12, 0x1000);
        wr32(m, p + 16, 0x10000);
        wr32(m, p + 20, 1);
    }
    return 28;
}

HLE("kernel32", GlobalAlloc, 2) {
    uint32_t r = guest_alloc(a[1], 16);
    if (!r) {
        set_last_error(c, ERROR_NOT_ENOUGH_MEMORY);
    }
    return r;
}

HLE("kernel32", GlobalFree, 1) { return guest_free(a[0]) ? 0 : a[0]; }
HLE("kernel32", GlobalLock, 1) { return a[0]; }

HLE("kernel32", GlobalUnlock, 1) {
    set_last_error(c, 0);
    return 0;
}

HLE("kernel32", GlobalMemoryStatus, 1) {
    uint32_t p = a[0];
    uint32_t v[8] = {32, 10, 256u << 20, 192u << 20, 512u << 20, 448u << 20, 0x7ffe0000u, 0x60000000u};
    std::memcpy(g_mem + p, v, sizeof v);
    return 0;
}

HLE("kernel32", InitializeCriticalSection, 1) {
    criticalSection(a[0]);
    return 0;
}

HLE("kernel32", DeleteCriticalSection, 1) {
    std::lock_guard lock(g_csMutex);
    g_criticalSections.erase(a[0]);
    return 0;
}

HLE("kernel32", EnterCriticalSection, 1) {
    criticalSection(a[0])->lock();
    return 0;
}

HLE("kernel32", LeaveCriticalSection, 1) {
    criticalSection(a[0])->unlock();
    return 0;
}

HLE("kernel32", TlsAlloc, 0) {
    std::lock_guard lock(g_tlsMutex);
    for (uint32_t i = 0; i < 64; i++) {
        if (!(g_tlsUsed & (uint64_t(1) << i))) {
            g_tlsUsed |= uint64_t(1) << i;
            wr32(m, c->fs_base + 0xe10 + 4 * i, 0);
            return i;
        }
    }
    return 0xffffffffu;
}

HLE("kernel32", TlsFree, 1) {
    std::lock_guard lock(g_tlsMutex);
    g_tlsUsed &= ~(uint64_t(1) << (a[0] & 63));
    return 1;
}

HLE("kernel32", TlsGetValue, 1) {
    set_last_error(c, 0);
    return a[0] < 64 ? rd32(m, c->fs_base + 0xe10 + 4 * a[0]) : 0;
}

HLE("kernel32", TlsSetValue, 2) {
    if (a[0] >= 64) {
        return 0;
    }
    wr32(m, c->fs_base + 0xe10 + 4 * a[0], a[1]);
    return 1;
}

HLE("kernel32", CreateEventA, 4) {
    auto e = std::make_shared<KEvent>();
    e->manual = a[1] != 0;
    e->signaled = a[2] != 0;
    set_last_error(c, 0);
    return handle_add(e);
}

HLE("kernel32", SetEvent, 1) {
    auto e = handle_as<KEvent>(a[0]);
    if (!e) {
        set_last_error(c, ERROR_INVALID_HANDLE);
        return 0;
    }
    {
        std::lock_guard lock(e->mu);
        e->signaled = true;
    }
    e->cv.notify_all();
    return 1;
}

HLE("kernel32", CreateMutexA, 3) {
    auto mu = std::make_shared<KMutex>();
    if (a[1]) {
        mu->owner = std::this_thread::get_id();
        mu->count = 1;
    }
    set_last_error(c, 0);
    return handle_add(mu);
}

HLE("kernel32", ReleaseMutex, 1) {
    auto mu = handle_as<KMutex>(a[0]);
    if (!mu) {
        set_last_error(c, ERROR_INVALID_HANDLE);
        return 0;
    }
    {
        std::lock_guard lock(mu->mu);
        if (mu->owner != std::this_thread::get_id() || mu->count == 0) {
            return 0;
        }
        mu->count--;
    }
    mu->cv.notify_all();
    return 1;
}

HLE("kernel32", WaitForSingleObject, 2) {
    auto w = handle_as<KWaitable>(a[0]);
    if (!w) {
        set_last_error(c, ERROR_INVALID_HANDLE);
        return 0xffffffffu;
    }
    std::unique_lock lock(w->mu);
    auto ready = [&] { return w->tryAcquire(); };
    if (a[1] == 0xffffffffu) {
        w->cv.wait(lock, ready);
    } else if (!w->cv.wait_for(lock, std::chrono::milliseconds(a[1]), ready)) {
        return 0x102;
    }
    return 0;
}

HLE("kernel32", CreateThread, 6) {
    if (a[4] & 4) {
        std::fprintf(stderr, "[kernel32] CreateThread with CREATE_SUSPENDED is not supported\n");
    }
    auto kt = std::make_shared<KThread>();
    uint32_t h = handle_add(kt);
    Ctx tc;
    uint32_t teb = thread_setup(tc, a[1]);
    uint32_t start = a[2];
    uint32_t param = a[3];
    if (a[5]) {
        wr32(m, a[5], rd32(m, teb + 0x24));
    }
    std::thread([kt, tc, start, param]() mutable {
        uint32_t code;
        try {
            push32(&tc, g_mem, param);
            push32(&tc, g_mem, 0);
            call_indirect(&tc, g_mem, start);
            code = tc.eax;
        } catch (const ThreadExit& e) {
            code = e.code;
        }
        {
            std::lock_guard lock(kt->mu);
            kt->done = true;
            kt->exitCode = code;
        }
        kt->cv.notify_all();
    }).detach();
    return h;
}

HLE("kernel32", SetThreadPriority, 2) { return 1; }

HLE("kernel32", Sleep, 1) {
    if (a[0] == 0) {
        std::this_thread::yield();
    } else {
        std::this_thread::sleep_for(std::chrono::milliseconds(a[0]));
    }
    return 0;
}

HLE("kernel32", QueryPerformanceFrequency, 1) {
    wr64(m, a[0], 1000000);
    return 1;
}

HLE("kernel32", QueryPerformanceCounter, 1) {
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    wr64(m, a[0], uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(now).count()));
    return 1;
}

HLE("kernel32", GetLocalTime, 1) {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm {};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    int64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
    uint16_t st[8] = {uint16_t(tm.tm_year + 1900), uint16_t(tm.tm_mon + 1), uint16_t(tm.tm_wday), uint16_t(tm.tm_mday),
                      uint16_t(tm.tm_hour), uint16_t(tm.tm_min), uint16_t(tm.tm_sec), uint16_t(ms)};
    std::memcpy(g_mem + a[0], st, sizeof st);
    return 0;
}

HLE("kernel32", GetTimeZoneInformation, 1) {
    std::memset(g_mem + a[0], 0, 172);
    return 0;
}

HLE("kernel32", FileTimeToLocalFileTime, 2) {
    writeFiletime(a[1], readFiletime(a[0]));
    return 1;
}

HLE("kernel32", LocalFileTimeToFileTime, 2) {
    writeFiletime(a[1], readFiletime(a[0]));
    return 1;
}

HLE("kernel32", FileTimeToSystemTime, 2) {
    writeSystemtime(a[1], readFiletime(a[0]));
    return 1;
}

HLE("kernel32", SystemTimeToFileTime, 2) {
    writeFiletime(a[1], readSystemtime(a[0]));
    return 1;
}

HLE("kernel32", DosDateTimeToFileTime, 3) {
    uint32_t date = a[0] & 0xffff;
    uint32_t time = a[1] & 0xffff;
    int64_t days = daysFromCivil(int((date >> 9) + 1980), (date >> 5) & 15, date & 31);
    int64_t secs = days * 86400 + (time >> 11) * 3600 + ((time >> 5) & 63) * 60 + (time & 31) * 2;
    writeFiletime(a[2], filetimeFromUnix(secs, 0));
    return 1;
}

HLE("kernel32", FileTimeToDosDateTime, 3) {
    int64_t secs = int64_t(readFiletime(a[0]) - kFiletimeUnixOffset) / 10000000;
    int64_t days = secs / 86400;
    int64_t rem = secs % 86400;
    int y;
    unsigned mo, d;
    civilFromDays(days, y, mo, d);
    if (a[1]) {
        wr16(m, a[1], uint16_t(((y - 1980) << 9) | (mo << 5) | d));
    }
    if (a[2]) {
        wr16(m, a[2], uint16_t(((rem / 3600) << 11) | ((rem / 60 % 60) << 5) | (rem % 60 / 2)));
    }
    return 1;
}

HLE("kernel32", CreateFileA, 7) {
    std::string name = gstring(a[0]);
    uint32_t access = a[1];
    uint32_t disposition = a[4];
    fs::path p = host_path(name);
    bool read = access & 0x80000000u;
    bool write = access & 0x40000000u;
    int flags = (read && write) ? O_RDWR : write ? O_WRONLY : O_RDONLY;
    switch (disposition) {
    case 1: flags |= O_CREAT | O_EXCL; break;
    case 2: flags |= O_CREAT | O_TRUNC; break;
    case 4: flags |= O_CREAT; break;
    case 5: flags |= O_TRUNC; break;
    default: break;
    }
    bool existed = fs::exists(p);
    int fd = openHost(p, flags);
    if (fd < 0) {
        uint32_t err = errnoToWin32(errno);
        if (err == ERROR_FILE_NOT_FOUND && !fs::exists(p.parent_path())) {
            err = ERROR_PATH_NOT_FOUND;
        }
        if (hle_trace()) {
            std::fprintf(stderr, "[kernel32] CreateFileA(\"%s\") -> %s failed: %s\n", name.c_str(), p.string().c_str(), std::strerror(errno));
        }
        set_last_error(c, err);
        return kInvalidHandle;
    }
    struct stat st {};
    fstat(fd, &st);
    if (S_ISDIR(st.st_mode)) {
        close(fd);
        set_last_error(c, ERROR_ACCESS_DENIED);
        return kInvalidHandle;
    }
    auto f = std::make_shared<KFile>();
    f->fd = fd;
    set_last_error(c, (existed && (disposition == 2 || disposition == 4)) ? ERROR_ALREADY_EXISTS : 0);
    return handle_add(f);
}

HLE("kernel32", ReadFile, 5) {
    auto f = handle_as<KFile>(a[0]);
    if (!f) {
        set_last_error(c, ERROR_INVALID_HANDLE);
        return 0;
    }
    uint32_t total = 0;
    if (!f->console) {
        while (total < a[2]) {
            ssize_t n = read(f->fd, g_mem + a[1] + total, a[2] - total);
            if (n <= 0) {
                break;
            }
            total += uint32_t(n);
        }
    }
    if (a[3]) {
        wr32(m, a[3], total);
    }
    return 1;
}

HLE("kernel32", WriteFile, 5) {
    auto f = handle_as<KFile>(a[0]);
    if (!f) {
        set_last_error(c, ERROR_INVALID_HANDLE);
        return 0;
    }
    uint32_t total = 0;
    int fd = f->console ? 2 : f->fd;
    while (total < a[2]) {
        ssize_t n = write(fd, g_mem + a[1] + total, a[2] - total);
        if (n <= 0) {
            break;
        }
        total += uint32_t(n);
    }
    if (a[3]) {
        wr32(m, a[3], total);
    }
    return 1;
}

HLE("kernel32", SetFilePointer, 4) {
    auto f = handle_as<KFile>(a[0]);
    if (!f || f->console) {
        set_last_error(c, ERROR_INVALID_HANDLE);
        return 0xffffffffu;
    }
    int64_t off = a[2] ? int64_t((uint64_t(rd32(m, a[2])) << 32) | a[1]) : int64_t(int32_t(a[1]));
    int whence = a[3] == 1 ? SEEK_CUR : a[3] == 2 ? SEEK_END : SEEK_SET;
    off_t r = lseek(f->fd, off, whence);
    if (r < 0) {
        set_last_error(c, 131);
        return 0xffffffffu;
    }
    if (a[2]) {
        wr32(m, a[2], uint32_t(uint64_t(r) >> 32));
    }
    set_last_error(c, 0);
    return uint32_t(r);
}

HLE("kernel32", FlushFileBuffers, 1) { return 1; }

HLE("kernel32", GetFileType, 1) {
    auto f = handle_as<KFile>(a[0]);
    if (!f) {
        set_last_error(c, ERROR_INVALID_HANDLE);
        return 0;
    }
    return f->console ? 2 : 1;
}

HLE("kernel32", CloseHandle, 1) {
    if (a[0] == 0xffffffffu || a[0] == 0xfffffffeu) {
        return 1;
    }
    if (!handle_close(a[0])) {
        set_last_error(c, ERROR_INVALID_HANDLE);
        return 0;
    }
    return 1;
}

HLE("kernel32", GetFileAttributesA, 1) {
    std::error_code ec;
    fs::file_status st = fs::status(host_path(gstring(a[0])), ec);
    if (!fs::exists(st)) {
        set_last_error(c, ERROR_FILE_NOT_FOUND);
        return 0xffffffffu;
    }
    return fs::is_directory(st) ? 0x10 : 0x20;
}

HLE("kernel32", SetFileAttributesA, 2) { return 1; }

HLE("kernel32", GetFileTime, 4) {
    auto f = handle_as<KFile>(a[0]);
    struct stat st {};
    if (!f || fstat(f->fd, &st) != 0) {
        set_last_error(c, ERROR_INVALID_HANDLE);
        return 0;
    }
    uint64_t ft = statFiletime(st);
    writeFiletime(a[1], ft);
    writeFiletime(a[2], ft);
    writeFiletime(a[3], ft);
    return 1;
}

HLE("kernel32", SetFileTime, 4) {
    auto f = handle_as<KFile>(a[0]);
    if (!f) {
        set_last_error(c, ERROR_INVALID_HANDLE);
        return 0;
    }
    if (a[3]) {
        int64_t t100 = int64_t(readFiletime(a[3]) - kFiletimeUnixOffset);
#ifdef _WIN32
        struct _utimbuf ut {t100 / 10000000, t100 / 10000000};
        _futime(f->fd, &ut);
#else
        struct timespec ts[2];
        ts[0].tv_sec = 0;
        ts[0].tv_nsec = UTIME_OMIT;
        ts[1].tv_sec = t100 / 10000000;
        ts[1].tv_nsec = (t100 % 10000000) * 100;
        futimens(f->fd, ts);
#endif
    }
    return 1;
}

HLE("kernel32", DeleteFileA, 1) {
    std::error_code ec;
    if (!fs::remove(host_path(gstring(a[0])), ec)) {
        set_last_error(c, ec ? win32Error(ec) : ERROR_FILE_NOT_FOUND);
        return 0;
    }
    return 1;
}

HLE("kernel32", MoveFileA, 2) {
    std::error_code ec;
    fs::rename(host_path(gstring(a[0])), host_path(gstring(a[1])), ec);
    if (ec) {
        set_last_error(c, win32Error(ec));
        return 0;
    }
    return 1;
}

HLE("kernel32", CreateDirectoryA, 2) {
    std::error_code ec;
    if (!fs::create_directory(host_path(gstring(a[0])), ec)) {
        set_last_error(c, ec ? win32Error(ec) : ERROR_ALREADY_EXISTS);
        return 0;
    }
    return 1;
}

HLE("kernel32", GetCurrentDirectoryA, 2) {
    std::string cwd = guest_cwd();
    if (a[0] < cwd.size() + 1) {
        return uint32_t(cwd.size() + 1);
    }
    return copy_string_out(a[1], a[0], cwd);
}

HLE("kernel32", SetCurrentDirectoryA, 1) {
    std::string path = gstring(a[0]);
    if (!fs::is_directory(host_path(path))) {
        set_last_error(c, ERROR_PATH_NOT_FOUND);
        return 0;
    }
    set_guest_cwd(path);
    return 1;
}

HLE("kernel32", GetFullPathNameA, 4) {
    std::string full = guest_full_path(gstring(a[0]));
    if (a[1] < full.size() + 1) {
        return uint32_t(full.size() + 1);
    }
    uint32_t n = copy_string_out(a[2], a[1], full);
    if (a[3]) {
        size_t slash = full.find_last_of('\\');
        wr32(m, a[3], slash == std::string::npos ? a[2] : a[2] + uint32_t(slash + 1));
    }
    return n;
}

HLE("kernel32", FindFirstFileA, 2) {
    std::string pattern = gstring(a[0]);
    for (char& ch : pattern) {
        if (ch == '/') {
            ch = '\\';
        }
    }
    size_t slash = pattern.find_last_of('\\');
    std::string dir = slash == std::string::npos ? "." : pattern.substr(0, slash + (slash == 0 ? 1 : 0));
    std::string mask = slash == std::string::npos ? pattern : pattern.substr(slash + 1);
    if (mask == "*.*") {
        mask = "*";
    }
    auto find = std::make_shared<KFind>();
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(host_path(dir), ec)) {
        std::string name = e.path().filename().string();
        if (name[0] == '.' && mask[0] != '.') {
            continue;
        }
        if (wildcardMatch(mask.c_str(), name.c_str())) {
            find->entries.push_back(e.path());
        }
    }
    std::sort(find->entries.begin(), find->entries.end());
    if (hle_trace()) {
        std::fprintf(stderr, "[kernel32] FindFirstFileA(\"%s\") -> %zu matches in %s\n", pattern.c_str(), find->entries.size(), host_path(dir).string().c_str());
    }
    if (find->entries.empty()) {
        set_last_error(c, ERROR_FILE_NOT_FOUND);
        return kInvalidHandle;
    }
    fillFindData(a[1], find->entries[0]);
    find->next = 1;
    return handle_add(find);
}

HLE("kernel32", FindNextFileA, 2) {
    auto find = handle_as<KFind>(a[0]);
    if (!find) {
        set_last_error(c, ERROR_INVALID_HANDLE);
        return 0;
    }
    if (find->next >= find->entries.size()) {
        set_last_error(c, ERROR_NO_MORE_FILES);
        return 0;
    }
    fillFindData(a[1], find->entries[find->next++]);
    return 1;
}

HLE("kernel32", FindClose, 1) { return handle_close(a[0]) ? 1 : 0; }
