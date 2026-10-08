// Test-only module markers. Never install these beside a game or runtime.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

extern "C" __declspec(dllexport) BOOL WINAPI bridgeTestMarker() {
    return TRUE;
}
