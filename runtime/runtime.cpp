#include "hle.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <limits>

void call_indirect(Ctx* c, uint8_t* m, uint32_t target) {
    if (hle_dispatch(c, m, target)) {
        return;
    }
    const FuncEntry* end = g_func_table + g_func_count;
    const FuncEntry* it = std::lower_bound(g_func_table, end, target, [](const FuncEntry& e, uint32_t a) { return e.addr < a; });
    if (it == end || it->addr != target) {
        std::fprintf(stderr, "call to unknown address 0x%08x (esp 0x%08x, return 0x%08x)\n", target, c->esp, rd32(m, c->esp));
        std::abort();
    }
    it->fn(c, m);
}

void unimplemented(Ctx* c, uint32_t addr, const char* what) {
    std::fprintf(stderr, "unimplemented instruction at 0x%08x: %s (esp 0x%08x)\n", addr, what, c->esp);
    std::abort();
}

void bad_jump(Ctx* c, uint32_t addr, uint32_t target) {
    std::fprintf(stderr, "jump from 0x%08x to unknown address 0x%08x (esp 0x%08x)\n", addr, target, c->esp);
    std::abort();
}

void div_error(Ctx* c) {
    std::fprintf(stderr, "integer divide error (eax 0x%08x, edx 0x%08x)\n", c->eax, c->edx);
    std::abort();
}

void cpuid(Ctx* c) {
    switch (c->eax) {
    case 0:
        c->eax = 1;
        c->ebx = 0x756e6547;
        c->edx = 0x49656e69;
        c->ecx = 0x6c65746e;
        break;
    case 1:
        c->eax = 0x652;
        c->ebx = 0;
        c->ecx = 0;
        c->edx = (1u << 0) | (1u << 23);
        break;
    default:
        c->eax = c->ebx = c->ecx = c->edx = 0;
        break;
    }
}

static_assert(std::numeric_limits<long double>::digits == 64, "the x87 emulation needs 80-bit long double");

long double f80_load(const uint8_t* p) {
    long double v = 0;
    std::memcpy(&v, p, 10);
    return v;
}

void f80_store(uint8_t* p, long double v) {
    std::memcpy(p, &v, 10);
}

void fpu_init(Ctx* c) {
    c->fcw = 0x37f;
    c->fsw = 0;
    c->ftop = 0;
    for (long double& r : c->st) {
        r = 0.0L;
    }
}

void fpu_save(Ctx* c, uint8_t* m, uint32_t ea) {
    std::memset(m + ea, 0, 108);
    wr16(m, ea, c->fcw);
    wr16(m, ea + 4, fstatus(c));
    for (int i = 0; i < 8; i++) {
        f80_store(m + ea + 28 + 10 * i, ST(i));
    }
    fpu_init(c);
}

void fpu_restore(Ctx* c, uint8_t* m, uint32_t ea) {
    c->fcw = rd16(m, ea);
    uint16_t sw = rd16(m, ea + 4);
    c->ftop = (sw >> 11) & 7;
    c->fsw = sw & ~0x3800;
    for (int i = 0; i < 8; i++) {
        ST(i) = f80_load(m + ea + 28 + 10 * i);
    }
}

void fpu_prem(Ctx* c) {
    long double a = ST(0);
    long double b = ST(1);
    long double q = std::trunc(a / b);
    ST(0) = std::fmod(a, b);
    c->fsw &= ~0x4700;
    if (std::fabs(q) < 9.2e18L) {
        uint64_t qi = uint64_t(std::fabs(q));
        if (qi & 1) c->fsw |= 0x0200;
        if (qi & 2) c->fsw |= 0x4000;
        if (qi & 4) c->fsw |= 0x0100;
    }
}
