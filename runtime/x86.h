#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <type_traits>

struct Ctx {
    union { uint32_t eax; uint16_t ax; struct { uint8_t al, ah; }; };
    union { uint32_t ecx; uint16_t cx; struct { uint8_t cl, ch; }; };
    union { uint32_t edx; uint16_t dx; struct { uint8_t dl, dh; }; };
    union { uint32_t ebx; uint16_t bx; struct { uint8_t bl, bh; }; };
    union { uint32_t esp; uint16_t sp; };
    union { uint32_t ebp; uint16_t bp; };
    union { uint32_t esi; uint16_t si; };
    union { uint32_t edi; uint16_t di; };
    uint8_t cf, pf, af, zf, sf, of, df;
    uint32_t eflags_sys;
    uint32_t fs_base;
    long double st[8];
    uint32_t ftop;
    uint16_t fcw, fsw;
    uint64_t mm[8];
};

using GuestFn = void (*)(Ctx* __restrict, uint8_t* __restrict);

struct FuncEntry {
    uint32_t addr;
    GuestFn fn;
};

extern const FuncEntry g_func_table[];
extern const uint32_t g_func_count;

void call_indirect(Ctx* c, uint8_t* m, uint32_t target);
[[noreturn]] void unimplemented(Ctx* c, uint32_t addr, const char* what);
[[noreturn]] void bad_jump(Ctx* c, uint32_t addr, uint32_t target);
[[noreturn]] void div_error(Ctx* c);
void cpuid(Ctx* c);
long double f80_load(const uint8_t* p);
void f80_store(uint8_t* p, long double v);
void fpu_init(Ctx* c);
void fpu_save(Ctx* c, uint8_t* m, uint32_t ea);
void fpu_restore(Ctx* c, uint8_t* m, uint32_t ea);
void fpu_prem(Ctx* c);

template <typename T>
inline T rd(const uint8_t* m, uint32_t a) {
    T v;
    std::memcpy(&v, m + a, sizeof(T));
    return v;
}

template <typename T>
inline void wr(uint8_t* m, uint32_t a, T v) {
    std::memcpy(m + a, &v, sizeof(T));
}

inline uint8_t rd8(const uint8_t* m, uint32_t a) { return m[a]; }
inline uint16_t rd16(const uint8_t* m, uint32_t a) { return rd<uint16_t>(m, a); }
inline uint32_t rd32(const uint8_t* m, uint32_t a) { return rd<uint32_t>(m, a); }
inline uint64_t rd64(const uint8_t* m, uint32_t a) { return rd<uint64_t>(m, a); }
inline void wr8(uint8_t* m, uint32_t a, uint8_t v) { m[a] = v; }
inline void wr16(uint8_t* m, uint32_t a, uint16_t v) { wr<uint16_t>(m, a, v); }
inline void wr32(uint8_t* m, uint32_t a, uint32_t v) { wr<uint32_t>(m, a, v); }
inline void wr64(uint8_t* m, uint32_t a, uint64_t v) { wr<uint64_t>(m, a, v); }
inline long double rdf32(const uint8_t* m, uint32_t a) { return rd<float>(m, a); }
inline long double rdf64(const uint8_t* m, uint32_t a) { return rd<double>(m, a); }
inline void wrf32(uint8_t* m, uint32_t a, long double v) { wr<float>(m, a, float(v)); }
inline void wrf64(uint8_t* m, uint32_t a, long double v) { wr<double>(m, a, double(v)); }

inline void push32(Ctx* c, uint8_t* m, uint32_t v) {
    c->esp -= 4;
    wr32(m, c->esp, v);
}

inline uint32_t pop32(Ctx* c, uint8_t* m) {
    uint32_t v = rd32(m, c->esp);
    c->esp += 4;
    return v;
}

enum : unsigned { F_CF = 0x1, F_PF = 0x4, F_AF = 0x10, F_ZF = 0x40, F_SF = 0x80, F_OF = 0x800, F_ALL = 0x8d5 };

template <typename T>
constexpr unsigned kBits = sizeof(T) * 8;

template <typename T>
inline uint8_t msb(T v) {
    return uint8_t((v >> (kBits<T> - 1)) & 1);
}

template <unsigned M, typename T>
inline void szp(Ctx* c, T r) {
    if constexpr (M & F_ZF) c->zf = r == 0;
    if constexpr (M & F_SF) c->sf = msb(r);
    if constexpr (M & F_PF) c->pf = uint8_t(!__builtin_parity(uint8_t(r)));
}

template <unsigned M, typename T>
inline T op_add(Ctx* c, T a, T b) {
    T r = T(a + b);
    if constexpr (M & F_CF) c->cf = r < a;
    if constexpr (M & F_OF) c->of = msb(T((a ^ r) & (b ^ r)));
    if constexpr (M & F_AF) c->af = ((a ^ b ^ r) >> 4) & 1;
    szp<M>(c, r);
    return r;
}

template <unsigned M, typename T>
inline T op_adc(Ctx* c, T a, T b) {
    T cin = c->cf;
    T r = T(a + b + cin);
    if constexpr (M & F_CF) c->cf = cin ? r <= a : r < a;
    if constexpr (M & F_OF) c->of = msb(T((a ^ r) & (b ^ r)));
    if constexpr (M & F_AF) c->af = ((a ^ b ^ r) >> 4) & 1;
    szp<M>(c, r);
    return r;
}

template <unsigned M, typename T>
inline T op_sub(Ctx* c, T a, T b) {
    T r = T(a - b);
    if constexpr (M & F_CF) c->cf = a < b;
    if constexpr (M & F_OF) c->of = msb(T((a ^ b) & (a ^ r)));
    if constexpr (M & F_AF) c->af = ((a ^ b ^ r) >> 4) & 1;
    szp<M>(c, r);
    return r;
}

template <unsigned M, typename T>
inline T op_sbb(Ctx* c, T a, T b) {
    T cin = c->cf;
    T r = T(a - b - cin);
    if constexpr (M & F_CF) c->cf = cin ? a <= b : a < b;
    if constexpr (M & F_OF) c->of = msb(T((a ^ b) & (a ^ r)));
    if constexpr (M & F_AF) c->af = ((a ^ b ^ r) >> 4) & 1;
    szp<M>(c, r);
    return r;
}

template <unsigned M, typename T>
inline T op_logic(Ctx* c, T r) {
    if constexpr (M & F_CF) c->cf = 0;
    if constexpr (M & F_OF) c->of = 0;
    if constexpr (M & F_AF) c->af = 0;
    szp<M>(c, r);
    return r;
}

template <unsigned M, typename T>
inline T op_inc(Ctx* c, T a) {
    T r = T(a + 1);
    if constexpr (M & F_OF) c->of = r == T(T(1) << (kBits<T> - 1));
    if constexpr (M & F_AF) c->af = (r & 0xf) == 0;
    szp<M>(c, r);
    return r;
}

template <unsigned M, typename T>
inline T op_dec(Ctx* c, T a) {
    T r = T(a - 1);
    if constexpr (M & F_OF) c->of = a == T(T(1) << (kBits<T> - 1));
    if constexpr (M & F_AF) c->af = (a & 0xf) == 0;
    szp<M>(c, r);
    return r;
}

template <unsigned M, typename T>
inline T op_neg(Ctx* c, T a) {
    T r = T(0 - a);
    if constexpr (M & F_CF) c->cf = a != 0;
    if constexpr (M & F_OF) c->of = a == T(T(1) << (kBits<T> - 1));
    if constexpr (M & F_AF) c->af = (a & 0xf) != 0;
    szp<M>(c, r);
    return r;
}

template <unsigned M, typename T>
inline T op_shl(Ctx* c, T a, uint8_t n) {
    n &= 31;
    if (n == 0) return a;
    uint64_t w = uint64_t(a) << n;
    T r = T(w);
    if constexpr (M & F_CF) c->cf = (w >> kBits<T>) & 1;
    if constexpr (M & F_OF) c->of = msb(r) ^ uint8_t((w >> kBits<T>) & 1);
    if constexpr (M & F_AF) c->af = 0;
    szp<M>(c, r);
    return r;
}

template <unsigned M, typename T>
inline T op_shr(Ctx* c, T a, uint8_t n) {
    n &= 31;
    if (n == 0) return a;
    T r = n < kBits<T> ? T(a >> n) : T(0);
    if constexpr (M & F_CF) c->cf = (uint64_t(a) >> (n - 1)) & 1;
    if constexpr (M & F_OF) c->of = msb(a);
    if constexpr (M & F_AF) c->af = 0;
    szp<M>(c, r);
    return r;
}

template <unsigned M, typename T>
inline T op_sar(Ctx* c, T a, uint8_t n) {
    using S = std::make_signed_t<T>;
    n &= 31;
    if (n == 0) return a;
    unsigned k = n < kBits<T> ? n : kBits<T> - 1;
    T r = T(S(a) >> k);
    if constexpr (M & F_CF) c->cf = uint8_t((S(a) >> (n - 1 < kBits<T> ? n - 1 : kBits<T> - 1)) & 1);
    if constexpr (M & F_OF) c->of = 0;
    if constexpr (M & F_AF) c->af = 0;
    szp<M>(c, r);
    return r;
}

template <unsigned M, typename T>
inline T op_rol(Ctx* c, T a, uint8_t n) {
    n &= 31;
    if (n == 0) return a;
    unsigned k = n % kBits<T>;
    T r = k ? T((a << k) | (a >> (kBits<T> - k))) : a;
    if constexpr (M & F_CF) c->cf = r & 1;
    if constexpr (M & F_OF) c->of = msb(r) ^ uint8_t(r & 1);
    return r;
}

template <unsigned M, typename T>
inline T op_ror(Ctx* c, T a, uint8_t n) {
    n &= 31;
    if (n == 0) return a;
    unsigned k = n % kBits<T>;
    T r = k ? T((a >> k) | (a << (kBits<T> - k))) : a;
    if constexpr (M & F_CF) c->cf = msb(r);
    if constexpr (M & F_OF) c->of = msb(r) ^ uint8_t((r >> (kBits<T> - 2)) & 1);
    return r;
}

template <unsigned M, typename T>
inline T op_rcl(Ctx* c, T a, uint8_t n) {
    n &= 31;
    if (n == 0) return a;
    constexpr unsigned W = kBits<T> + 1;
    unsigned k = n % W;
    uint64_t mask = (uint64_t(1) << W) - 1;
    uint64_t w = (uint64_t(c->cf) << kBits<T>) | a;
    if (k) w = ((w << k) | (w >> (W - k))) & mask;
    T r = T(w);
    c->cf = (w >> kBits<T>) & 1;
    if constexpr (M & F_OF) c->of = msb(r) ^ c->cf;
    return r;
}

template <unsigned M, typename T>
inline T op_rcr(Ctx* c, T a, uint8_t n) {
    n &= 31;
    if (n == 0) return a;
    constexpr unsigned W = kBits<T> + 1;
    unsigned k = n % W;
    uint64_t mask = (uint64_t(1) << W) - 1;
    uint64_t w = (uint64_t(c->cf) << kBits<T>) | a;
    if (k) w = ((w >> k) | (w << (W - k))) & mask;
    T r = T(w);
    c->cf = (w >> kBits<T>) & 1;
    if constexpr (M & F_OF) c->of = msb(r) ^ uint8_t((r >> (kBits<T> - 2)) & 1);
    return r;
}

template <unsigned M, typename T>
inline T op_shld(Ctx* c, T a, T b, uint8_t n) {
    n &= 31;
    if (n == 0) return a;
    T r = T((uint64_t(a) << n) | (uint64_t(b) >> (kBits<T> - n)));
    if constexpr (M & F_CF) c->cf = (uint64_t(a) >> (kBits<T> - n)) & 1;
    if constexpr (M & F_OF) c->of = msb(r) ^ msb(a);
    if constexpr (M & F_AF) c->af = 0;
    szp<M>(c, r);
    return r;
}

template <unsigned M, typename T>
inline T op_shrd(Ctx* c, T a, T b, uint8_t n) {
    n &= 31;
    if (n == 0) return a;
    T r = T((uint64_t(a) >> n) | (uint64_t(b) << (kBits<T> - n)));
    if constexpr (M & F_CF) c->cf = (uint64_t(a) >> (n - 1)) & 1;
    if constexpr (M & F_OF) c->of = msb(r) ^ msb(a);
    if constexpr (M & F_AF) c->af = 0;
    szp<M>(c, r);
    return r;
}

template <unsigned M, typename T>
inline T op_imul(Ctx* c, T a, T b) {
    using S = std::make_signed_t<T>;
    int64_t w = int64_t(S(a)) * int64_t(S(b));
    T r = T(w);
    if constexpr (M & (F_CF | F_OF)) {
        uint8_t o = w != int64_t(S(r));
        c->cf = o;
        c->of = o;
    }
    szp<M & (F_ZF | F_SF | F_PF)>(c, r);
    return r;
}

template <unsigned M>
inline void op_mul8(Ctx* c, uint8_t b) {
    c->ax = uint16_t(c->al * b);
    if constexpr (M & (F_CF | F_OF)) { c->cf = c->of = c->ah != 0; }
}

template <unsigned M>
inline void op_mul16(Ctx* c, uint16_t b) {
    uint32_t r = uint32_t(c->ax) * b;
    c->ax = uint16_t(r);
    c->dx = uint16_t(r >> 16);
    if constexpr (M & (F_CF | F_OF)) { c->cf = c->of = c->dx != 0; }
}

template <unsigned M>
inline void op_mul32(Ctx* c, uint32_t b) {
    uint64_t r = uint64_t(c->eax) * b;
    c->eax = uint32_t(r);
    c->edx = uint32_t(r >> 32);
    if constexpr (M & (F_CF | F_OF)) { c->cf = c->of = c->edx != 0; }
}

template <unsigned M>
inline void op_imul8(Ctx* c, uint8_t b) {
    int16_t r = int16_t(int8_t(c->al) * int8_t(b));
    c->ax = uint16_t(r);
    if constexpr (M & (F_CF | F_OF)) { c->cf = c->of = r != int8_t(r); }
}

template <unsigned M>
inline void op_imul16(Ctx* c, uint16_t b) {
    int32_t r = int32_t(int16_t(c->ax)) * int16_t(b);
    c->ax = uint16_t(r);
    c->dx = uint16_t(uint32_t(r) >> 16);
    if constexpr (M & (F_CF | F_OF)) { c->cf = c->of = r != int16_t(r); }
}

template <unsigned M>
inline void op_imul32(Ctx* c, uint32_t b) {
    int64_t r = int64_t(int32_t(c->eax)) * int32_t(b);
    c->eax = uint32_t(r);
    c->edx = uint32_t(uint64_t(r) >> 32);
    if constexpr (M & (F_CF | F_OF)) { c->cf = c->of = r != int32_t(r); }
}

inline void op_div8(Ctx* c, uint8_t b) {
    if (b == 0) div_error(c);
    uint16_t n = c->ax;
    uint16_t q = n / b;
    if (q > 0xff) div_error(c);
    c->al = uint8_t(q);
    c->ah = uint8_t(n % b);
}

inline void op_div16(Ctx* c, uint16_t b) {
    if (b == 0) div_error(c);
    uint32_t n = (uint32_t(c->dx) << 16) | c->ax;
    uint32_t q = n / b;
    if (q > 0xffff) div_error(c);
    c->ax = uint16_t(q);
    c->dx = uint16_t(n % b);
}

inline void op_div32(Ctx* c, uint32_t b) {
    if (b == 0) div_error(c);
    uint64_t n = (uint64_t(c->edx) << 32) | c->eax;
    uint64_t q = n / b;
    if (q > 0xffffffffu) div_error(c);
    c->eax = uint32_t(q);
    c->edx = uint32_t(n % b);
}

inline void op_idiv8(Ctx* c, uint8_t b) {
    if (b == 0) div_error(c);
    int32_t n = int16_t(c->ax);
    int32_t q = n / int8_t(b);
    if (q != int8_t(q)) div_error(c);
    c->al = uint8_t(q);
    c->ah = uint8_t(n % int8_t(b));
}

inline void op_idiv16(Ctx* c, uint16_t b) {
    if (b == 0) div_error(c);
    int32_t n = int32_t((uint32_t(c->dx) << 16) | c->ax);
    int64_t q = int64_t(n) / int16_t(b);
    if (q != int16_t(q)) div_error(c);
    c->ax = uint16_t(q);
    c->dx = uint16_t(int64_t(n) % int16_t(b));
}

inline void op_idiv32(Ctx* c, uint32_t b) {
    if (b == 0) div_error(c);
    int64_t n = int64_t((uint64_t(c->edx) << 32) | c->eax);
    if (n == INT64_MIN && int32_t(b) == -1) div_error(c);
    int64_t q = n / int32_t(b);
    if (q != int32_t(q)) div_error(c);
    c->eax = uint32_t(q);
    c->edx = uint32_t(n % int32_t(b));
}

template <typename T>
inline T acc(Ctx* c) {
    if constexpr (sizeof(T) == 1) return c->al;
    else if constexpr (sizeof(T) == 2) return c->ax;
    else return c->eax;
}

template <typename T>
inline void set_acc(Ctx* c, T v) {
    if constexpr (sizeof(T) == 1) c->al = v;
    else if constexpr (sizeof(T) == 2) c->ax = v;
    else c->eax = v;
}

template <typename T>
inline uint32_t sdelta(Ctx* c) {
    return c->df ? uint32_t(0) - uint32_t(sizeof(T)) : uint32_t(sizeof(T));
}

template <typename T>
inline void s_movs(Ctx* c, uint8_t* m) {
    wr<T>(m, c->edi, rd<T>(m, c->esi));
    c->esi += sdelta<T>(c);
    c->edi += sdelta<T>(c);
}

template <typename T>
inline void s_rep_movs(Ctx* c, uint8_t* m) {
    uint64_t n = uint64_t(c->ecx) * sizeof(T);
    if (!c->df && n && (uint64_t(c->edi) + n <= c->esi || uint64_t(c->esi) + n <= c->edi)) {
        std::memcpy(m + c->edi, m + c->esi, n);
        c->esi += uint32_t(n);
        c->edi += uint32_t(n);
        c->ecx = 0;
        return;
    }
    for (; c->ecx; c->ecx--) s_movs<T>(c, m);
}

template <typename T>
inline void s_stos(Ctx* c, uint8_t* m) {
    wr<T>(m, c->edi, acc<T>(c));
    c->edi += sdelta<T>(c);
}

template <typename T>
inline void s_rep_stos(Ctx* c, uint8_t* m) {
    if constexpr (sizeof(T) == 1) {
        if (!c->df && c->ecx) {
            std::memset(m + c->edi, c->al, c->ecx);
            c->edi += c->ecx;
            c->ecx = 0;
            return;
        }
    }
    for (; c->ecx; c->ecx--) s_stos<T>(c, m);
}

template <typename T>
inline void s_lods(Ctx* c, uint8_t* m) {
    set_acc<T>(c, rd<T>(m, c->esi));
    c->esi += sdelta<T>(c);
}

template <typename T>
inline void s_rep_lods(Ctx* c, uint8_t* m) {
    for (; c->ecx; c->ecx--) s_lods<T>(c, m);
}

template <typename T>
inline void s_scas(Ctx* c, uint8_t* m) {
    op_sub<F_ALL, T>(c, acc<T>(c), rd<T>(m, c->edi));
    c->edi += sdelta<T>(c);
}

template <typename T, bool Z>
inline void s_rep_scas(Ctx* c, uint8_t* m) {
    while (c->ecx) {
        s_scas<T>(c, m);
        c->ecx--;
        if (c->zf != Z) break;
    }
}

template <typename T>
inline void s_cmps(Ctx* c, uint8_t* m) {
    op_sub<F_ALL, T>(c, rd<T>(m, c->esi), rd<T>(m, c->edi));
    c->esi += sdelta<T>(c);
    c->edi += sdelta<T>(c);
}

template <typename T, bool Z>
inline void s_rep_cmps(Ctx* c, uint8_t* m) {
    while (c->ecx) {
        s_cmps<T>(c, m);
        c->ecx--;
        if (c->zf != Z) break;
    }
}

inline uint32_t get_eflags(Ctx* c) {
    return 0x202u | c->cf | (c->pf << 2) | (c->af << 4) | (c->zf << 6) | (c->sf << 7) | (uint32_t(c->df) << 10) | (uint32_t(c->of) << 11) | c->eflags_sys;
}

inline void set_eflags(Ctx* c, uint32_t f) {
    c->cf = f & 1;
    c->pf = (f >> 2) & 1;
    c->af = (f >> 4) & 1;
    c->zf = (f >> 6) & 1;
    c->sf = (f >> 7) & 1;
    c->df = (f >> 10) & 1;
    c->of = (f >> 11) & 1;
    c->eflags_sys = f & 0x240000u;
}

inline void op_sahf(Ctx* c) {
    uint8_t f = c->ah;
    c->cf = f & 1;
    c->pf = (f >> 2) & 1;
    c->af = (f >> 4) & 1;
    c->zf = (f >> 6) & 1;
    c->sf = (f >> 7) & 1;
}

inline void op_lahf(Ctx* c) {
    c->ah = uint8_t(0x02 | c->cf | (c->pf << 2) | (c->af << 4) | (c->zf << 6) | (c->sf << 7));
}

#define ST(i) c->st[(c->ftop + (i)) & 7]

inline void fpush(Ctx* c, long double v) {
    c->ftop = (c->ftop - 1) & 7;
    c->st[c->ftop] = v;
}

inline void fpop(Ctx* c) {
    c->ftop = (c->ftop + 1) & 7;
}

inline uint16_t fstatus(Ctx* c) {
    return uint16_t((c->fsw & ~0x3800) | ((c->ftop & 7) << 11));
}

inline void fcompare(Ctx* c, long double a, long double b) {
    uint16_t s = c->fsw & ~0x4500;
    if (std::isnan(a) || std::isnan(b)) s |= 0x4500;
    else if (a < b) s |= 0x0100;
    else if (a == b) s |= 0x4000;
    c->fsw = s;
}

inline long double fround(Ctx* c, long double v) {
    switch ((c->fcw >> 10) & 3) {
    case 0: return std::nearbyint(v);
    case 1: return std::floor(v);
    case 2: return std::ceil(v);
    default: return std::trunc(v);
    }
}

inline uint16_t fist16(Ctx* c, long double v) {
    long double r = fround(c, v);
    return r >= -32768.0L && r <= 32767.0L ? uint16_t(int16_t(r)) : uint16_t(0x8000);
}

inline uint32_t fist32(Ctx* c, long double v) {
    long double r = fround(c, v);
    return r >= -2147483648.0L && r <= 2147483647.0L ? uint32_t(int32_t(r)) : 0x80000000u;
}

inline uint64_t fist64(Ctx* c, long double v) {
    long double r = fround(c, v);
    return r >= -9223372036854775808.0L && r < 9223372036854775808.0L ? uint64_t(int64_t(r)) : 0x8000000000000000ull;
}

namespace mmx {

template <typename L, typename F>
inline uint64_t lanes(uint64_t a, uint64_t b, F f) {
    using U = std::make_unsigned_t<L>;
    constexpr unsigned W = sizeof(L) * 8;
    uint64_t r = 0;
    for (unsigned i = 0; i < 64; i += W) {
        L x = L(U(a >> i));
        L y = L(U(b >> i));
        r |= uint64_t(U(f(x, y))) << i;
    }
    return r;
}

inline uint64_t paddb(uint64_t a, uint64_t b) { return lanes<uint8_t>(a, b, [](uint8_t x, uint8_t y) { return uint8_t(x + y); }); }
inline uint64_t paddw(uint64_t a, uint64_t b) { return lanes<uint16_t>(a, b, [](uint16_t x, uint16_t y) { return uint16_t(x + y); }); }
inline uint64_t paddd(uint64_t a, uint64_t b) { return lanes<uint32_t>(a, b, [](uint32_t x, uint32_t y) { return uint32_t(x + y); }); }
inline uint64_t paddusb(uint64_t a, uint64_t b) { return lanes<uint8_t>(a, b, [](uint8_t x, uint8_t y) { return uint8_t(x + y > 0xff ? 0xff : x + y); }); }
inline uint64_t paddusw(uint64_t a, uint64_t b) { return lanes<uint16_t>(a, b, [](uint16_t x, uint16_t y) { return uint16_t(x + y > 0xffff ? 0xffff : x + y); }); }
inline uint64_t psubb(uint64_t a, uint64_t b) { return lanes<uint8_t>(a, b, [](uint8_t x, uint8_t y) { return uint8_t(x - y); }); }
inline uint64_t psubw(uint64_t a, uint64_t b) { return lanes<uint16_t>(a, b, [](uint16_t x, uint16_t y) { return uint16_t(x - y); }); }
inline uint64_t psubd(uint64_t a, uint64_t b) { return lanes<uint32_t>(a, b, [](uint32_t x, uint32_t y) { return uint32_t(x - y); }); }
inline uint64_t psubusb(uint64_t a, uint64_t b) { return lanes<uint8_t>(a, b, [](uint8_t x, uint8_t y) { return uint8_t(x > y ? x - y : 0); }); }
inline uint64_t psubusw(uint64_t a, uint64_t b) { return lanes<uint16_t>(a, b, [](uint16_t x, uint16_t y) { return uint16_t(x > y ? x - y : 0); }); }
inline uint64_t pmullw(uint64_t a, uint64_t b) { return lanes<int16_t>(a, b, [](int16_t x, int16_t y) { return int16_t(int32_t(x) * y); }); }
inline uint64_t pmulhw(uint64_t a, uint64_t b) { return lanes<int16_t>(a, b, [](int16_t x, int16_t y) { return int16_t((int32_t(x) * y) >> 16); }); }
inline uint64_t pcmpeqb(uint64_t a, uint64_t b) { return lanes<uint8_t>(a, b, [](uint8_t x, uint8_t y) { return uint8_t(x == y ? 0xff : 0); }); }
inline uint64_t pcmpeqw(uint64_t a, uint64_t b) { return lanes<uint16_t>(a, b, [](uint16_t x, uint16_t y) { return uint16_t(x == y ? 0xffff : 0); }); }
inline uint64_t pcmpeqd(uint64_t a, uint64_t b) { return lanes<uint32_t>(a, b, [](uint32_t x, uint32_t y) { return uint32_t(x == y ? 0xffffffffu : 0); }); }
inline uint64_t pcmpgtb(uint64_t a, uint64_t b) { return lanes<int8_t>(a, b, [](int8_t x, int8_t y) { return int8_t(x > y ? -1 : 0); }); }
inline uint64_t pcmpgtw(uint64_t a, uint64_t b) { return lanes<int16_t>(a, b, [](int16_t x, int16_t y) { return int16_t(x > y ? -1 : 0); }); }
inline uint64_t pcmpgtd(uint64_t a, uint64_t b) { return lanes<int32_t>(a, b, [](int32_t x, int32_t y) { return int32_t(x > y ? -1 : 0); }); }
inline uint64_t pmaddwd(uint64_t a, uint64_t b) {
    uint64_t r = 0;
    for (unsigned i = 0; i < 2; i++) {
        int32_t s = int32_t(int16_t(a >> (32 * i))) * int16_t(b >> (32 * i)) + int32_t(int16_t(a >> (32 * i + 16))) * int16_t(b >> (32 * i + 16));
        r |= uint64_t(uint32_t(s)) << (32 * i);
    }
    return r;
}

inline uint64_t psllw(uint64_t a, uint64_t n) { return n > 15 ? 0 : lanes<uint16_t>(a, 0, [n](uint16_t x, uint16_t) { return uint16_t(x << n); }); }
inline uint64_t pslld(uint64_t a, uint64_t n) { return n > 31 ? 0 : lanes<uint32_t>(a, 0, [n](uint32_t x, uint32_t) { return uint32_t(x << n); }); }
inline uint64_t psllq(uint64_t a, uint64_t n) { return n > 63 ? 0 : a << n; }
inline uint64_t psrlw(uint64_t a, uint64_t n) { return n > 15 ? 0 : lanes<uint16_t>(a, 0, [n](uint16_t x, uint16_t) { return uint16_t(x >> n); }); }
inline uint64_t psrld(uint64_t a, uint64_t n) { return n > 31 ? 0 : lanes<uint32_t>(a, 0, [n](uint32_t x, uint32_t) { return uint32_t(x >> n); }); }
inline uint64_t psrlq(uint64_t a, uint64_t n) { return n > 63 ? 0 : a >> n; }
inline uint64_t psraw(uint64_t a, uint64_t n) { unsigned k = n > 15 ? 15 : unsigned(n); return lanes<int16_t>(a, 0, [k](int16_t x, int16_t) { return int16_t(x >> k); }); }
inline uint64_t psrad(uint64_t a, uint64_t n) { unsigned k = n > 31 ? 31 : unsigned(n); return lanes<int32_t>(a, 0, [k](int32_t x, int32_t) { return int32_t(x >> k); }); }

inline uint8_t sat_u8(int16_t v) { return uint8_t(v < 0 ? 0 : v > 255 ? 255 : v); }
inline int8_t sat_s8(int16_t v) { return int8_t(v < -128 ? -128 : v > 127 ? 127 : v); }
inline int16_t sat_s16(int32_t v) { return int16_t(v < -32768 ? -32768 : v > 32767 ? 32767 : v); }

inline uint64_t packuswb(uint64_t a, uint64_t b) {
    uint64_t r = 0;
    for (unsigned i = 0; i < 4; i++) {
        r |= uint64_t(sat_u8(int16_t(a >> (16 * i)))) << (8 * i);
        r |= uint64_t(sat_u8(int16_t(b >> (16 * i)))) << (8 * i + 32);
    }
    return r;
}

inline uint64_t packsswb(uint64_t a, uint64_t b) {
    uint64_t r = 0;
    for (unsigned i = 0; i < 4; i++) {
        r |= uint64_t(uint8_t(sat_s8(int16_t(a >> (16 * i))))) << (8 * i);
        r |= uint64_t(uint8_t(sat_s8(int16_t(b >> (16 * i))))) << (8 * i + 32);
    }
    return r;
}

inline uint64_t packssdw(uint64_t a, uint64_t b) {
    uint64_t r = 0;
    for (unsigned i = 0; i < 2; i++) {
        r |= uint64_t(uint16_t(sat_s16(int32_t(a >> (32 * i))))) << (16 * i);
        r |= uint64_t(uint16_t(sat_s16(int32_t(b >> (32 * i))))) << (16 * i + 32);
    }
    return r;
}

template <typename L>
inline uint64_t unpack(uint64_t a, uint64_t b, unsigned half) {
    constexpr unsigned W = sizeof(L) * 8;
    constexpr unsigned N = 32 / W;
    uint64_t mask = W == 64 ? ~uint64_t(0) : (uint64_t(1) << W) - 1;
    uint64_t r = 0;
    for (unsigned i = 0; i < N; i++) {
        unsigned src = (half * N + i) * W;
        r |= ((a >> src) & mask) << (2 * i * W);
        r |= ((b >> src) & mask) << ((2 * i + 1) * W);
    }
    return r;
}

inline uint64_t punpcklbw(uint64_t a, uint64_t b) { return unpack<uint8_t>(a, b, 0); }
inline uint64_t punpcklwd(uint64_t a, uint64_t b) { return unpack<uint16_t>(a, b, 0); }
inline uint64_t punpckldq(uint64_t a, uint64_t b) { return unpack<uint32_t>(a, b, 0); }
inline uint64_t punpckhbw(uint64_t a, uint64_t b) { return unpack<uint8_t>(a, b, 1); }
inline uint64_t punpckhwd(uint64_t a, uint64_t b) { return unpack<uint16_t>(a, b, 1); }
inline uint64_t punpckhdq(uint64_t a, uint64_t b) { return unpack<uint32_t>(a, b, 1); }

}
