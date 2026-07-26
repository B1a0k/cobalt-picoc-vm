#include <windows.h>
#include <file.h>
#include <tlhelp32.h>
#include <msvcrt.h>
#include <beacon.h>

char *security_images[] = {
    "MsMpEng.exe",
    "SenseIR.exe",
    "CSFalconService.exe",
    "SentinelAgent.exe",
    "CylanceSvc.exe",
    "WRSA.exe"
};

int is_security_image(char *image)
{
    int index;
    for (index = 0;
         index < (int)(sizeof(security_images) / sizeof(security_images[0]));
         index++) {
        if (_stricmp(image, security_images[index]) == 0)
            return 1;
    }
    return 0;
}

void main()
{
    HANDLE snapshot;
    PROCESSENTRY32 entry;
    DWORD matches;

    snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        BeaconPrintf(CALLBACK_ERROR, "process snapshot failed");
        return;
    }
    entry.dwSize = sizeof(entry);
    matches = 0;
    if (Process32First(snapshot, &entry)) {
        do {
            if (is_security_image(entry.szExeFile)) {
                BeaconPrintf(CALLBACK_OUTPUT,
                    "security_process pid=%lu image=%s",
                    entry.th32ProcessID, entry.szExeFile);
                matches++;
            }
        } while (Process32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);
    BeaconPrintf(CALLBACK_OUTPUT,
        "security process heuristic matches=%lu", matches);
}

main();
