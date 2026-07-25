#include <stdio.h>

struct Pair {
    int first;
    int second;
};

struct Box {
    struct Pair pairs[2];
    int tail;
};

union Number {
    int integer;
    unsigned char bytes[4];
};

int main(void)
{
    struct Pair complete = {1, 2};
    struct Pair partial = {3};
    struct Pair pairs[2] = {{4, 5}, {6, 7}};
    struct Box box = {{{8, 9}, {10, 11}}, 12};
    union Number number = {0x01020304};
    struct Pair copied;

    copied = complete;
    printf("%d %d %d %d\n",
        complete.first,
        complete.second,
        partial.first,
        partial.second);
    printf("%d %d %d %d\n",
        pairs[0].second,
        pairs[1].first,
        box.pairs[1].second,
        box.tail);
    printf("%d %u\n", copied.second, number.bytes[0]);
    return 0;
}
