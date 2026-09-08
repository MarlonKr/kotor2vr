#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include "probe.hpp"

static_assert(sizeof(void*) == 4,
              "kotor2vr_game32 must be built for Win32/x86, never x64");

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        k2vr::game32::SetSelfModule(module);
        // No worker is created under the loader lock. LoadLibrary success did
        // not prove that the previous fire-and-forget worker ever ran. The
        // injector must call K2VR_ProbeBootstrap after LoadLibrary returns and
        // check its result. All I/O, hashing, and engine reads then happen
        // outside DllMain and are acknowledged synchronously.
    }
    return TRUE;
}

#endif
