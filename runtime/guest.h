#pragma once

#include "x86.h"

#include <string>

constexpr uint32_t kHleBase = 0xf0000000u;

extern uint8_t* g_mem;

void guest_memory_init();
uint32_t guest_alloc(uint32_t size, uint32_t align);
uint32_t guest_alloc_at(uint32_t addr, uint32_t size);
bool guest_free(uint32_t addr);
bool guest_region(uint32_t addr, uint32_t& base, uint32_t& size);
void guest_dump(const std::string& path);

inline char* gstr(uint32_t a) {
    return reinterpret_cast<char*>(g_mem + a);
}

std::string gstring(uint32_t a);
uint32_t guest_strdup(const std::string& s);
uint32_t copy_string_out(uint32_t dst, uint32_t cap, const std::string& s);
