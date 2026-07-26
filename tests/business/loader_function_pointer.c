#include <windows.h>
#include <beacon.h>

size_t (cdecl *resolved_strlen)(char *text);
void *module;
void *address;

module = GetModuleHandleA("msvcrt.dll");
if (module == NULL)
    module = LoadLibraryA("msvcrt.dll");
address = GetProcAddress(module, "strlen");
if (address == NULL) {
    BeaconPrintf(CALLBACK_ERROR, "strlen resolution failed");
}
else {
    resolved_strlen = address;
    BeaconPrintf(CALLBACK_OUTPUT, "resolved strlen result=%u",
        (unsigned int)resolved_strlen("beacon"));
}
