#include <windows.h>
#include <winreg.h>
#include <advapi.h>
#include <beacon.h>

/* Top-level execution: audit component exports and registered providers. */
HMODULE amsi;
void *initialize_address;
void *scan_address;
HKEY providers;
char provider_name[80];
DWORD provider_length;
DWORD provider_count;
LONG registry_status;

amsi = LoadLibraryA("amsi.dll");
initialize_address = amsi == NULL
    ? NULL : GetProcAddress(amsi, "AmsiInitialize");
scan_address = amsi == NULL
    ? NULL : GetProcAddress(amsi, "AmsiScanBuffer");

provider_count = 0;
registry_status = RegOpenKeyExA(HKEY_LOCAL_MACHINE,
    "SOFTWARE\\Microsoft\\AMSI\\Providers", 0, KEY_READ, &providers);
if (registry_status == ERROR_SUCCESS) {
    provider_length = sizeof(provider_name);
    while (provider_count < 32 &&
           RegEnumKeyExA(providers, provider_count, provider_name,
               &provider_length, NULL, NULL, NULL, NULL) == ERROR_SUCCESS) {
        BeaconPrintf(CALLBACK_OUTPUT, "AMSI provider[%lu]=%s",
            provider_count, provider_name);
        provider_count++;
        provider_length = sizeof(provider_name);
    }
    RegCloseKey(providers);
}

BeaconPrintf(CALLBACK_OUTPUT,
    "amsi_dll=%s initialize=%s scan_buffer=%s providers=%lu",
    amsi != NULL ? "present" : "missing",
    initialize_address != NULL ? "present" : "missing",
    scan_address != NULL ? "present" : "missing",
    provider_count);
if (amsi != NULL)
    FreeLibrary(amsi);
