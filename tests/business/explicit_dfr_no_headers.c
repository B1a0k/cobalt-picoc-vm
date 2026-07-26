KERNEL32$GetCurrentProcessId: stdcall u32 ();
KERNEL32$GetTickCount: stdcall u32 ();
BEACON$BeaconPrintf: cdecl void (i32, cstr, ...);

unsigned int pid;
unsigned int tick;

pid = KERNEL32$GetCurrentProcessId();
tick = GetTickCount();
BeaconPrintf(0, "pid=%u tick=%u", pid, tick);
