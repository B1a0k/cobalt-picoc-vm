#include <stdio.h>

int main(void)
{
    int i;
    int j;
    int total = 0;

    for (i = 0; i < 5; i++) {
        if (i == 1)
            continue;
        switch (i) {
        case 0:
            total += 10;
            break;
        case 2:
            total += 20;
        case 3:
            total += 3;
            break;
        default:
            total += 1;
        }
    }

    i = 0;
again:
    i++;
    if (i < 3)
        goto again;

    j = 0;
    do {
        int inner = 0;
        while (1) {
            inner++;
            if (inner == 2)
                break;
        }
        j += inner;
    } while (j < 4);

    printf("%d %d %d\n", total, i, j);
    return 0;
}
