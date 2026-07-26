#include <windows.h>

size_t (cdecl *my_strlen)(char *text);

int main(void)
{
    void *module = GetModuleHandleA("msvcrt.dll");
    void *proc;
    if (module == NULL)
        module = LoadLibraryA("msvcrt.dll");
    proc = GetProcAddress(module, "strlen");
    if (proc == NULL)
        return -1;
    my_strlen = proc;
    return (int)my_strlen("hello");
}
