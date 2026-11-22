#include "win32.h"

#include <cstdio>

HLE("advapi32", RegOpenKeyExA, 5) {
    if (hle_trace()) {
        std::fprintf(stderr, "[advapi32] RegOpenKeyExA(0x%x, \"%s\") -> not found\n", a[0], gstring(a[1]).c_str());
    }
    return ERROR_FILE_NOT_FOUND;
}

HLE("advapi32", RegCreateKeyExA, 9) {
    if (a[7]) {
        wr32(m, a[7], 0x1000);
    }
    if (a[8]) {
        wr32(m, a[8], 1);
    }
    return 0;
}

HLE("advapi32", RegQueryValueExA, 6) { return ERROR_FILE_NOT_FOUND; }
HLE("advapi32", RegSetValueExA, 6) { return 0; }
HLE("advapi32", RegCloseKey, 1) { return 0; }
