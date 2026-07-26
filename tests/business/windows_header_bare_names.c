#include <windows.h>
#include <beacon.h>

DWORD pid;
DWORD tick;

pid = GetCurrentProcessId();
tick = GetTickCount();
BeaconPrintf(CALLBACK_OUTPUT, "bare header calls: pid=%lu tick=%lu",
    pid, tick);
