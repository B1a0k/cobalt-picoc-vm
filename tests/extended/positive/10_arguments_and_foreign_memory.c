#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    int index;
    int total = 0;
    printf("argc=%d\n", argc);
    for (index = 0; index < argc; index++) {
        total += strlen(argv[index]);
        printf("%d:%s\n", index, argv[index]);
    }
    printf("total=%d\n", total);
    return 0;
}
