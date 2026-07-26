#include <windows.h>
#include <file.h>
#include <tlhelp32.h>
#include <beacon.h>

/*
 * Bounded process inventory. The cap is deliberate so generated scripts do
 * not flood the client with one event per process.
 */
HANDLE snapshot;
PROCESSENTRY32 entry;
DWORD process_count;
DWORD shown;

snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
if (snapshot == INVALID_HANDLE_VALUE) {
    BeaconPrintf(CALLBACK_ERROR, "CreateToolhelp32Snapshot failed");
}
else {
    entry.dwSize = sizeof(entry);
    process_count = 0;
    shown = 0;
    if (Process32First(snapshot, &entry)) {
        do {
            process_count++;
            if (shown < 5) {
                BeaconPrintf(CALLBACK_OUTPUT, "pid=%lu parent=%lu image=%s",
                    entry.th32ProcessID, entry.th32ParentProcessID,
                    entry.szExeFile);
                shown++;
            }
        } while (Process32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);
    BeaconPrintf(CALLBACK_OUTPUT, "processes=%lu shown=%lu",
        process_count, shown);
}
