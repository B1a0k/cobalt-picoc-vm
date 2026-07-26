#include <windows.h>
#include <advapi.h>
#include <beacon.h>

typedef struct _SCRIPT_TOKEN_ELEVATION {
    DWORD TokenIsElevated;
} SCRIPT_TOKEN_ELEVATION;

HANDLE token;
SCRIPT_TOKEN_ELEVATION elevation;
DWORD returned;

token = NULL;
returned = 0;
if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
    BeaconPrintf(CALLBACK_ERROR, "OpenProcessToken failed");
}
else {
    if (GetTokenInformation(token, TokenElevation, &elevation,
            sizeof(elevation), &returned)) {
        BeaconPrintf(CALLBACK_OUTPUT,
            "token elevated=%lu information_bytes=%lu",
            elevation.TokenIsElevated, returned);
    }
    else {
        BeaconPrintf(CALLBACK_ERROR, "GetTokenInformation failed");
    }
    CloseHandle(token);
}
