#include <windows.h>
#include <winreg.h>
#include <advapi.h>
#include <beacon.h>

int read_uac_dword(char *name, DWORD *value)
{
    HKEY key;
    DWORD type;
    DWORD size;
    LONG status;

    status = RegOpenKeyExA(HKEY_LOCAL_MACHINE,
        "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System",
        0, KEY_READ, &key);
    if (status != ERROR_SUCCESS)
        return 0;
    type = 0;
    size = sizeof(*value);
    status = RegQueryValueExA(key, name, NULL, &type,
        (BYTE *)value, &size);
    RegCloseKey(key);
    return status == ERROR_SUCCESS && type == REG_DWORD;
}

void main()
{
    DWORD enable_lua;
    DWORD admin_prompt;
    DWORD secure_desktop;

    if (!read_uac_dword("EnableLUA", &enable_lua)) {
        BeaconPrintf(CALLBACK_ERROR, "unable to read UAC policy");
        return;
    }
    read_uac_dword("ConsentPromptBehaviorAdmin", &admin_prompt);
    read_uac_dword("PromptOnSecureDesktop", &secure_desktop);
    BeaconPrintf(CALLBACK_OUTPUT,
        "UAC EnableLUA=%lu admin_prompt=%lu secure_desktop=%lu",
        enable_lua, admin_prompt, secure_desktop);
}

main();
