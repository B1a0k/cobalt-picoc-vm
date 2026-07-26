#include <windows.h>
#include <beacon.h>

/*
 * Safe inventory example: it only reads information about the current host
 * and returns one bounded status line.
 */
char computer_name[64];
DWORD computer_name_length;
MEMORYSTATUSEX memory_status;
DWORD process_id;
DWORD uptime_ms;

computer_name_length = sizeof(computer_name);
computer_name[0] = 0;
memory_status.dwLength = sizeof(memory_status);
process_id = GetCurrentProcessId();
uptime_ms = GetTickCount();

if (!GetComputerNameA(computer_name, &computer_name_length))
    computer_name[0] = 0;

if (GlobalMemoryStatusEx(&memory_status)) {
    BeaconPrintf(CALLBACK_OUTPUT,
        "host=%s pid=%lu uptime_ms=%lu memory_load=%lu",
        computer_name, process_id, uptime_ms, memory_status.dwMemoryLoad);
}
else {
    BeaconPrintf(CALLBACK_ERROR,
        "host=%s pid=%lu uptime_ms=%lu memory query failed",
        computer_name, process_id, uptime_ms);
}
