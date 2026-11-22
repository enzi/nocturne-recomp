#pragma once

#include "guest.h"

#include <initializer_list>
#include <string>

struct HleArgs {
    Ctx* c;
    uint8_t* m;
    uint32_t operator[](int i) const { return rd32(m, c->esp + 4 + 4 * i); }
};

using HleFn = uint32_t (*)(Ctx* c, uint8_t* m, const HleArgs& a);

bool hle_register(const char* dll, const char* name, int nargs, HleFn fn);
uint32_t hle_thunk(const std::string& dll, const std::string& name);
bool hle_dispatch(Ctx* c, uint8_t* m, uint32_t target);
uint32_t call_guest(Ctx* c, uint8_t* m, uint32_t fn, std::initializer_list<uint32_t> args);
bool hle_trace();

#define HLE(dll, name, nargs)                                                         \
    static uint32_t hle_##name(Ctx* c, uint8_t* m, const HleArgs& a);                 \
    [[maybe_unused]] static const bool hle_reg_##name = hle_register(dll, #name, nargs, hle_##name); \
    static uint32_t hle_##name([[maybe_unused]] Ctx* c, [[maybe_unused]] uint8_t* m, [[maybe_unused]] const HleArgs& a)

#define HLE_ORDINAL(dll, ident, ordinal, nargs)                                       \
    static uint32_t hle_##ident(Ctx* c, uint8_t* m, const HleArgs& a);                \
    [[maybe_unused]] static const bool hle_reg_##ident = hle_register(dll, "#" #ordinal, nargs, hle_##ident); \
    static uint32_t hle_##ident([[maybe_unused]] Ctx* c, [[maybe_unused]] uint8_t* m, [[maybe_unused]] const HleArgs& a)

#define HLE_CDECL(dll, name, nargs)                                                   \
    static uint32_t hle_##name(Ctx* c, uint8_t* m, const HleArgs& a);                 \
    [[maybe_unused]] static const bool hle_reg_##name = hle_register(dll, #name, -(nargs), hle_##name); \
    static uint32_t hle_##name([[maybe_unused]] Ctx* c, [[maybe_unused]] uint8_t* m, [[maybe_unused]] const HleArgs& a)
