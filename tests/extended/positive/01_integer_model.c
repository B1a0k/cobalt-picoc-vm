#include <stdio.h>

int main(void)
{
    signed char sc = 0xff;
    unsigned char uc = 0x1ff;
    signed short ss = 0xffff;
    unsigned short us = 0x1ffff;
    int negative = -7;
    unsigned int maximum = 0xffffffffU;
    long long wide = 0x123456789abcdefLL;
    unsigned long long unsigned_wide = 0xfedcba9876543210ULL;

    printf("%d %u %d %u\n", sc, uc, ss, us);
    printf("%d %u %u\n",
        negative / 3,
        maximum > 1U,
        (unsigned int)-1 > 1U);
    printf("%lld %llu\n", wide, unsigned_wide);
    printf("%u %u %u %u %d\n",
        0x80000000U >> 31,
        0x12345678U & 0xffU,
        7U % 4U,
        3U << 4,
        -8 >> 2);
    printf("%u %u %u %u %u %u %u\n",
        7U / 3U,
        1U < 2U,
        2U <= 2U,
        3U >= 2U,
        ~0U,
        0x10U | 3U,
        0x1fU ^ 3U);
    printf("%d %d %d %d\n",
        -2 < -1,
        -2 <= -2,
        3 > 2,
        3 >= 3);
    return 0;
}
