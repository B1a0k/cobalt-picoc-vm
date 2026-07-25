#include <stdio.h>

int main(void)
{
    int values[4] = {1, 2, 3, 4};
    int *left = &values[0];
    int *right = &values[3];
    int index = 0;
    int result;

    result = (values[index++] += 4, values[index++] *= 3, index);
    *left = *right;
    right = 1 + left;

    printf("%d %d %d %d %d\n",
        values[0],
        values[1],
        values[2],
        *right,
        result);
    return 0;
}
