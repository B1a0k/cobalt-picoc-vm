#include <stdio.h>
#include <string.h>

struct Packet {
    unsigned int command;
    unsigned short length;
    unsigned char flags;
    char payload[9];
};

int main(void)
{
    struct Packet source = {0x11223344U, 6, 0x5a, "beacon"};
    struct Packet copy;
    unsigned char *raw;
    char rendered[64];

    memset(&copy, 0, sizeof(copy));
    memcpy(&copy, &source, sizeof(source));
    raw = (unsigned char *)&copy;
    sprintf(rendered, "%08x:%u:%02x:%s",
        copy.command,
        copy.length,
        copy.flags,
        copy.payload);

    printf("%s\n", rendered);
    printf("%u %u %d\n",
        raw[0],
        raw[3],
        memcmp(&source, &copy, sizeof(source)) == 0);
    return 0;
}
