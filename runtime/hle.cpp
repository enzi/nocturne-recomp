#include "hle.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <map>
#include <mutex>
#include <vector>

namespace {

struct Registered {
    int nargs;
    HleFn fn;
};

struct Thunk {
    std::string dll;
    std::string name;
    int nargs;
    HleFn fn;
};

std::map<std::string, Registered>& registry() {
    static std::map<std::string, Registered> r;
    return r;
}

std::mutex g_thunkMutex;
std::deque<Thunk> g_thunks;
std::map<std::string, uint32_t> g_thunkByKey;

std::string normalizeDll(std::string dll) {
    for (char& ch : dll) {
        ch = char(std::tolower(static_cast<unsigned char>(ch)));
    }
    if (dll.size() > 4 && dll.compare(dll.size() - 4, 4, ".dll") == 0) {
        dll.resize(dll.size() - 4);
    }
    return dll;
}

std::string key(const std::string& dll, const std::string& name) {
    return normalizeDll(dll) + "!" + name;
}

}

bool hle_trace() {
    static const bool on = std::getenv("NOC_TRACE") != nullptr;
    return on;
}

bool hle_register(const char* dll, const char* name, int nargs, HleFn fn) {
    registry()[key(dll, name)] = {nargs, fn};
    return true;
}

uint32_t hle_thunk(const std::string& dll, const std::string& name) {
    std::lock_guard lock(g_thunkMutex);
    std::string k = key(dll, name);
    if (auto it = g_thunkByKey.find(k); it != g_thunkByKey.end()) {
        return it->second;
    }
    Thunk t{normalizeDll(dll), name, 0, nullptr};
    if (auto it = registry().find(k); it != registry().end()) {
        t.nargs = it->second.nargs;
        t.fn = it->second.fn;
    }
    uint32_t addr = kHleBase + 16 * static_cast<uint32_t>(g_thunks.size());
    g_thunks.push_back(t);
    g_thunkByKey[k] = addr;
    return addr;
}

bool hle_dispatch(Ctx* c, uint8_t* m, uint32_t target) {
    if (target < kHleBase) {
        return false;
    }
    const Thunk* t;
    {
        std::lock_guard lock(g_thunkMutex);
        uint32_t idx = (target - kHleBase) / 16;
        if ((target - kHleBase) % 16 || idx >= g_thunks.size()) {
            std::fprintf(stderr, "call to invalid thunk address 0x%08x\n", target);
            std::abort();
        }
        t = &g_thunks[idx];
    }
    uint32_t ret = rd32(m, c->esp);
    if (!t->fn) {
        std::fprintf(stderr, "unimplemented import %s!%s (called from 0x%08x)\n", t->dll.c_str(), t->name.c_str(), ret);
        std::abort();
    }
    HleArgs a{c, m};
    if (hle_trace()) {
        std::fprintf(stderr, "[hle] %s!%s(", t->dll.c_str(), t->name.c_str());
        for (int i = 0; i < (t->nargs < 0 ? -t->nargs : t->nargs); i++) {
            std::fprintf(stderr, "%s0x%x", i ? ", " : "", a[i]);
        }
        std::fprintf(stderr, ") from 0x%08x\n", ret);
    }
    int nargs = t->nargs;
    uint32_t r = t->fn(c, m, a);
    c->eax = r;
    c->esp += 4 + 4 * (nargs > 0 ? nargs : 0);
    return true;
}

uint32_t call_guest(Ctx* c, uint8_t* m, uint32_t fn, std::initializer_list<uint32_t> args) {
    uint32_t esp = c->esp;
    std::vector<uint32_t> v(args);
    for (auto it = v.rbegin(); it != v.rend(); ++it) {
        push32(c, m, *it);
    }
    push32(c, m, 0);
    call_indirect(c, m, fn);
    c->esp = esp;
    return c->eax;
}
