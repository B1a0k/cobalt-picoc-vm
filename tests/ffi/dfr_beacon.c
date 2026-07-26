BEACON$BeaconDataParse: cdecl void (ptr, cstr, i32);
BEACON$BeaconDataInt: cdecl i32 (ptr);
BEACON$BeaconDataExtract: cdecl cstr (ptr, ptr);

int main(void)
{
    void *parser = NULL;
    BeaconDataParse(parser, NULL, 0);
    return BEACON$BeaconDataInt(parser);
}
