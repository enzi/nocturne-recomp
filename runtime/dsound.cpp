#include "win32.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>

namespace {

constexpr uint32_t DS_OK = 0;
constexpr uint32_t E_NOINTERFACE = 0x80004002u;
constexpr uint32_t DSERR_INVALIDPARAM = 0x80070057u;
constexpr uint32_t DSERR_NODRIVER = 0x88780078u;

constexpr uint32_t DSBCAPS_PRIMARYBUFFER = 0x1;
constexpr uint32_t DSBPLAY_LOOPING = 0x1;
constexpr uint32_t DSBSTATUS_PLAYING = 0x1;
constexpr uint32_t DSBSTATUS_LOOPING = 0x4;
constexpr uint32_t DSBLOCK_FROMWRITECURSOR = 0x1;
constexpr uint32_t DSBLOCK_ENTIREBUFFER = 0x2;

constexpr uint32_t DSCAPS_PRIMARY_AND_SECONDARY_FORMATS = 0x0f1f;

struct Format {
    uint16_t tag = 1;
    uint16_t channels = 2;
    uint32_t rate = 22050;
    uint16_t bits = 8;

    uint16_t blockAlign() const { return uint16_t(channels * bits / 8); }
};

struct Buffer {
    bool primary = false;
    Format format;
    uint32_t data = 0;
    uint32_t size = 0;
    uint32_t cursor = 0;
    bool playing = false;
    bool looping = false;
    uint32_t refs = 1;
    SDL_AudioStream* stream = nullptr;
};

std::mutex g_mutex;
std::map<uint32_t, Buffer> g_buffers;
SDL_AudioDeviceID g_device = 0;

uint32_t makeVtable(const char* prefix, std::initializer_list<const char*> methods) {
    uint32_t vt = guest_alloc(4 * uint32_t(methods.size()), 4);
    uint32_t i = 0;
    for (const char* name : methods) {
        wr32(g_mem, vt + 4 * i++, hle_thunk("dsound", std::string(prefix) + "_" + name));
    }
    return vt;
}

uint32_t soundVtable() {
    static uint32_t vt = makeVtable("DS", {"QueryInterface", "AddRef", "Release", "CreateSoundBuffer", "GetCaps",
        "DuplicateSoundBuffer", "SetCooperativeLevel", "Compact", "GetSpeakerConfig", "SetSpeakerConfig", "Initialize"});
    return vt;
}

uint32_t bufferVtable() {
    static uint32_t vt = makeVtable("DSB", {"QueryInterface", "AddRef", "Release", "GetCaps", "GetCurrentPosition",
        "GetFormat", "GetVolume", "GetPan", "GetFrequency", "GetStatus", "Initialize", "Lock", "Play", "SetCurrentPosition",
        "SetFormat", "SetVolume", "SetPan", "SetFrequency", "Stop", "Unlock", "Restore"});
    return vt;
}

uint32_t newObject(uint32_t vtable) {
    uint32_t obj = guest_alloc(16, 4);
    wr32(g_mem, obj, vtable);
    return obj;
}

bool openDevice() {
    if (g_device) {
        return true;
    }
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        std::fprintf(stderr, "[dsound] SDL audio init failed: %s\n", SDL_GetError());
        return false;
    }
    g_device = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, nullptr);
    if (!g_device) {
        std::fprintf(stderr, "[dsound] no audio device: %s\n", SDL_GetError());
        return false;
    }
    std::fprintf(stderr, "[dsound] audio device opened (%s driver)\n", SDL_GetCurrentAudioDriver());
    return true;
}

Format readFormat(uint32_t p) {
    Format f;
    f.tag = rd16(g_mem, p);
    f.channels = rd16(g_mem, p + 2);
    f.rate = rd32(g_mem, p + 4);
    f.bits = rd16(g_mem, p + 14);
    return f;
}

void writeFormat(uint32_t p, uint32_t size, const Format& f) {
    uint8_t w[18] = {};
    uint32_t avg = f.rate * f.blockAlign();
    uint16_t align = f.blockAlign();
    std::memcpy(w, &f.tag, 2);
    std::memcpy(w + 2, &f.channels, 2);
    std::memcpy(w + 4, &f.rate, 4);
    std::memcpy(w + 8, &avg, 4);
    std::memcpy(w + 12, &align, 2);
    std::memcpy(w + 14, &f.bits, 2);
    std::memcpy(g_mem + p, w, std::min<uint32_t>(size, sizeof w));
}

void SDLCALL feed(void* user, SDL_AudioStream* stream, int need, int) {
    std::lock_guard lock(g_mutex);
    auto it = g_buffers.find(uint32_t(uintptr_t(user)));
    if (it == g_buffers.end()) {
        return;
    }
    Buffer& b = it->second;
    uint32_t align = std::max<uint32_t>(b.format.blockAlign(), 1);
    uint32_t left = uint32_t(need + int(align) - 1) / align * align;
    while (b.playing && left > 0) {
        uint32_t n = std::min(left, b.size - b.cursor);
        SDL_PutAudioStreamData(stream, g_mem + b.data + b.cursor, int(n));
        b.cursor += n;
        left -= n;
        if (b.cursor >= b.size) {
            b.cursor = 0;
            b.playing = b.looping;
        }
    }
}

uint32_t createBuffer(uint32_t desc) {
    Buffer b;
    uint32_t flags = rd32(g_mem, desc + 4);
    if (flags & DSBCAPS_PRIMARYBUFFER) {
        b.primary = true;
    } else {
        b.size = rd32(g_mem, desc + 8);
        b.format = readFormat(rd32(g_mem, desc + 16));
        if (b.size == 0 || b.format.tag != 1 || (b.format.bits != 8 && b.format.bits != 16)) {
            return 0;
        }
        b.data = guest_alloc(b.size, 16);
        SDL_AudioSpec spec{b.format.bits == 8 ? SDL_AUDIO_U8 : SDL_AUDIO_S16LE, b.format.channels, int(b.format.rate)};
        b.stream = SDL_CreateAudioStream(&spec, nullptr);
    }
    uint32_t obj = newObject(bufferVtable());
    if (b.stream) {
        SDL_SetAudioStreamGetCallback(b.stream, feed, reinterpret_cast<void*>(uintptr_t(obj)));
        SDL_BindAudioStream(g_device, b.stream);
    }
    std::lock_guard lock(g_mutex);
    g_buffers[obj] = b;
    return obj;
}

}

HLE_ORDINAL("dsound", DirectSoundCreate, 1, 3) {
    if (a[1]) {
        wr32(m, a[1], 0);
    }
    if (!openDevice()) {
        return DSERR_NODRIVER;
    }
    if (a[1]) {
        wr32(m, a[1], newObject(soundVtable()));
    }
    return DS_OK;
}

HLE_ORDINAL("dsound", DirectSoundEnumerateA, 2, 2) {
    static const uint32_t desc = [] {
        const char text[] = "Primary Sound Driver";
        uint32_t p = guest_alloc(sizeof text + 1, 4);
        std::memcpy(g_mem + p, text, sizeof text);
        return p;
    }();
    call_guest(c, m, a[0], {0, desc, desc + uint32_t(std::strlen("Primary Sound Driver")), a[1]});
    return DS_OK;
}

HLE("dsound", DS_QueryInterface, 3) {
    wr32(m, a[2], 0);
    return E_NOINTERFACE;
}

HLE("dsound", DS_AddRef, 1) { return 2; }
HLE("dsound", DS_Release, 1) { return 0; }
HLE("dsound", DS_SetCooperativeLevel, 3) { return DS_OK; }

HLE("dsound", DS_GetCaps, 2) {
    uint32_t p = a[1];
    uint32_t size = rd32(m, p);
    std::memset(g_mem + p, 0, size);
    wr32(m, p, size);
    wr32(m, p + 4, DSCAPS_PRIMARY_AND_SECONDARY_FORMATS);
    wr32(m, p + 8, 100);
    wr32(m, p + 12, 100000);
    wr32(m, p + 16, 1);
    return DS_OK;
}

HLE("dsound", DS_CreateSoundBuffer, 4) {
    uint32_t obj = createBuffer(a[1]);
    wr32(m, a[2], obj);
    return obj ? DS_OK : DSERR_INVALIDPARAM;
}

HLE("dsound", DSB_QueryInterface, 3) {
    wr32(m, a[2], 0);
    return E_NOINTERFACE;
}

HLE("dsound", DSB_AddRef, 1) {
    std::lock_guard lock(g_mutex);
    auto it = g_buffers.find(a[0]);
    return it == g_buffers.end() ? 0 : ++it->second.refs;
}

HLE("dsound", DSB_Release, 1) {
    Buffer b;
    {
        std::lock_guard lock(g_mutex);
        auto it = g_buffers.find(a[0]);
        if (it == g_buffers.end()) {
            return 0;
        }
        if (--it->second.refs) {
            return it->second.refs;
        }
        b = it->second;
        g_buffers.erase(it);
    }
    if (b.stream) {
        SDL_DestroyAudioStream(b.stream);
    }
    if (b.data) {
        guest_free(b.data);
    }
    guest_free(a[0]);
    return 0;
}

HLE("dsound", DSB_GetCurrentPosition, 3) {
    std::lock_guard lock(g_mutex);
    auto it = g_buffers.find(a[0]);
    if (it == g_buffers.end() || it->second.size == 0) {
        return DSERR_INVALIDPARAM;
    }
    const Buffer& b = it->second;
    if (a[1]) {
        wr32(m, a[1], b.cursor);
    }
    if (a[2]) {
        uint32_t lead = b.format.rate / 100 * b.format.blockAlign();
        wr32(m, a[2], (b.cursor + lead) % b.size);
    }
    return DS_OK;
}

HLE("dsound", DSB_GetFormat, 4) {
    std::lock_guard lock(g_mutex);
    auto it = g_buffers.find(a[0]);
    if (it == g_buffers.end()) {
        return DSERR_INVALIDPARAM;
    }
    if (a[1]) {
        writeFormat(a[1], a[2], it->second.format);
    }
    if (a[3]) {
        wr32(m, a[3], 18);
    }
    return DS_OK;
}

HLE("dsound", DSB_SetFormat, 2) {
    std::lock_guard lock(g_mutex);
    auto it = g_buffers.find(a[0]);
    if (it == g_buffers.end() || !it->second.primary) {
        return DSERR_INVALIDPARAM;
    }
    it->second.format = readFormat(a[1]);
    std::fprintf(stderr, "[dsound] primary format %u Hz, %u channels, %u bits\n", it->second.format.rate, it->second.format.channels, it->second.format.bits);
    return DS_OK;
}

HLE("dsound", DSB_GetStatus, 2) {
    std::lock_guard lock(g_mutex);
    auto it = g_buffers.find(a[0]);
    if (it == g_buffers.end()) {
        return DSERR_INVALIDPARAM;
    }
    const Buffer& b = it->second;
    wr32(m, a[1], b.playing ? DSBSTATUS_PLAYING | (b.looping ? DSBSTATUS_LOOPING : 0) : 0);
    return DS_OK;
}

HLE("dsound", DSB_Lock, 8) {
    std::lock_guard lock(g_mutex);
    auto it = g_buffers.find(a[0]);
    if (it == g_buffers.end() || it->second.size == 0) {
        return DSERR_INVALIDPARAM;
    }
    const Buffer& b = it->second;
    uint32_t offset = a[1];
    uint32_t bytes = a[2];
    if (a[7] & DSBLOCK_FROMWRITECURSOR) {
        offset = b.cursor;
    }
    if (a[7] & DSBLOCK_ENTIREBUFFER) {
        bytes = b.size;
    }
    if (offset >= b.size || bytes > b.size) {
        return DSERR_INVALIDPARAM;
    }
    uint32_t first = std::min(bytes, b.size - offset);
    wr32(m, a[3], b.data + offset);
    wr32(m, a[4], first);
    if (a[5]) {
        wr32(m, a[5], bytes > first ? b.data : 0);
    }
    if (a[6]) {
        wr32(m, a[6], bytes - first);
    }
    return DS_OK;
}

HLE("dsound", DSB_Unlock, 5) { return DS_OK; }

HLE("dsound", DSB_Play, 4) {
    std::lock_guard lock(g_mutex);
    auto it = g_buffers.find(a[0]);
    if (it == g_buffers.end()) {
        return DSERR_INVALIDPARAM;
    }
    it->second.playing = true;
    it->second.looping = (a[3] & DSBPLAY_LOOPING) != 0;
    return DS_OK;
}

HLE("dsound", DSB_SetCurrentPosition, 2) {
    std::lock_guard lock(g_mutex);
    auto it = g_buffers.find(a[0]);
    if (it == g_buffers.end() || a[1] >= std::max<uint32_t>(it->second.size, 1)) {
        return DSERR_INVALIDPARAM;
    }
    it->second.cursor = a[1];
    return DS_OK;
}

HLE("dsound", DSB_Stop, 1) {
    std::lock_guard lock(g_mutex);
    auto it = g_buffers.find(a[0]);
    if (it == g_buffers.end()) {
        return DSERR_INVALIDPARAM;
    }
    it->second.playing = false;
    return DS_OK;
}

HLE("dsound", DSB_Restore, 1) { return DS_OK; }
