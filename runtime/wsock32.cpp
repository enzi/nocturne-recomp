#include "win32.h"

#include <cstdio>

namespace {

constexpr uint32_t WSASYSNOTREADY = 10091;

}

HLE_ORDINAL("wsock32", WSAStartup, 115, 2) {
    std::fprintf(stderr, "[wsock32] WSAStartup -> networking not available\n");
    return WSASYSNOTREADY;
}

HLE_ORDINAL("wsock32", WSACleanup, 116, 0) { return 0; }
HLE_ORDINAL("wsock32", htons, 9, 1) { return uint16_t((a[0] >> 8) | (a[0] << 8)); }
HLE_ORDINAL("wsock32", ntohs, 15, 1) { return uint16_t((a[0] >> 8) | (a[0] << 8)); }
