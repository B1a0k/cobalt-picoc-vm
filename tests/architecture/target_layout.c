#include <stdio.h>

struct Mixed {
    char prefix;
    void *pointer;
    char suffix;
};

struct Node {
    int value;
    struct Node *next;
};

union Wide {
    double number;
    void *pointer;
    char bytes[3];
};

int layout_probe(int first, void *pointer, int last)
{
    return first + (pointer != 0) + last;
}

int main(void)
{
    void *pointers[3];
    printf(
        "%d %d %d %d %d\n",
        (int)sizeof(void *),
        (int)sizeof(struct Mixed),
        (int)sizeof(struct Node),
        (int)sizeof(union Wide),
        (int)sizeof(pointers));
    return layout_probe(2, 0, 3) - 5;
}
