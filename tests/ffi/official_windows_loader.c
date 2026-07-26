#include <windows.h>

int main(void)
{
    HMODULE module = GetModuleHandleA("kernel32.dll");
    if (module == NULL)
        module = LoadLibraryA("kernel32.dll");
    return GetProcAddress(module, "GetTickCount") != NULL;
}
