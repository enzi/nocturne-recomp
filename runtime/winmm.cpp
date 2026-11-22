#include "win32.h"

#include <SDL3/SDL.h>

#include <cstdio>

namespace {

constexpr uint32_t MMSYSERR_BADDEVICEID = 2;
constexpr uint32_t JOYERR_UNPLUGGED = 167;
constexpr uint32_t MCIERR_DEVICE_NOT_INSTALLED = 306;

}

HLE("winmm", timeGetTime, 0) { return uint32_t(SDL_GetTicks()); }
HLE("winmm", timeBeginPeriod, 1) { return 0; }
HLE("winmm", timeEndPeriod, 1) { return 0; }

HLE("winmm", joyGetDevCapsA, 3) { return JOYERR_UNPLUGGED; }
HLE("winmm", joyGetPos, 2) { return JOYERR_UNPLUGGED; }

HLE("winmm", mciSendStringA, 4) {
    std::fprintf(stderr, "[winmm] mciSendStringA(\"%s\") -> not available\n", gstring(a[0]).c_str());
    return MCIERR_DEVICE_NOT_INSTALLED;
}

HLE("winmm", mciGetErrorStringA, 3) {
    copy_string_out(a[1], a[2], "MCI is not available");
    return 1;
}

HLE("winmm", waveOutGetDevCapsA, 3) { return MMSYSERR_BADDEVICEID; }
HLE("winmm", waveOutOpen, 6) { return MMSYSERR_BADDEVICEID; }
HLE("winmm", waveOutPrepareHeader, 3) { return MMSYSERR_BADDEVICEID; }
HLE("winmm", waveOutUnprepareHeader, 3) { return MMSYSERR_BADDEVICEID; }
HLE("winmm", waveOutWrite, 3) { return MMSYSERR_BADDEVICEID; }
HLE("winmm", waveOutReset, 1) { return MMSYSERR_BADDEVICEID; }
HLE("winmm", waveOutClose, 1) { return MMSYSERR_BADDEVICEID; }
