#include <beacon.h>

int main(void)
{
    datap parser;
    BeaconDataParse(&parser, NULL, 0);
    return BeaconDataLength(&parser);
}
