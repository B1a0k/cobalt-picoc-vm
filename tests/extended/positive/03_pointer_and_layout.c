#include <stdio.h>

struct Point {
    int x;
    int y;
};

struct Outer {
    char tag;
    struct Point points[2];
};

int main(void)
{
    int values[5] = {10, 20, 30, 40, 50};
    int *pointer = values;
    struct Outer object;

    printf("%d\n", *(pointer + 2));
    pointer += 3;
    printf("%d\n", *pointer);
    pointer--;
    printf("%d\n", *pointer);
    printf("%lld\n",
        (long long)(&values[4] - &values[1]));

    object.tag = 'Q';
    object.points[0].x = 4;
    object.points[0].y = 5;
    object.points[1].x = 8;
    object.points[1].y = 9;
    printf("%c %d %d %d\n",
        object.tag,
        object.points[0].x,
        object.points[1].y,
        sizeof(struct Outer));
    printf("%d %d\n",
        &values[0] < &values[4],
        (&values[2] == pointer));
    return 0;
}
