BEACON$BeaconPrintf: cdecl void (i32, cstr, ...);

int main(void)
{
    BeaconPrintf(0, "%s:%d", "value", 7);
    return 0;
}
