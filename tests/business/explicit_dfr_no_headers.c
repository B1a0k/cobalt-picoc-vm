KERNEL32$GetCurrentProcessId: u32 ();
KERNEL32$GetTickCount: u32 ();
BEACON$BeaconPrintf: cdecl void (i32, cstr, ...);

void main()
{
    unsigned int process_id;
    unsigned int uptime;

    process_id = KERNEL32$GetCurrentProcessId();
    uptime = KERNEL32$GetTickCount();
    BEACON$BeaconPrintf(0,
        "explicit DFR pid=%u uptime_ms=%u", process_id, uptime);
}

main();
