#include <stdio.h>

int main(void)
{
    int a;
    int b;
    int c;
    int x;
    int y;
    int z;
    int post;
    int pre;
    double selected;

    a = b = c = 7;
    a *= 3;
    b += 5;
    c -= 2;
    a /= 2;
    b %= 5;
    printf("%d %d %d\n", a, b, c);

    x = 1;
    post = x++;
    pre = ++x;
    printf("%d %d %d\n", x, post, pre);

    b = 2;
    a = (b = 4, b + 3);
    printf("%d %d\n", a, b);

    x = 0;
    y = 0 && (x = 5);
    z = 1 || (x = 6);
    printf("%d %d %d\n", x, y, z);

    selected = 1 ? 2.5 : 3;
    a = 0 ? 2.5 : 3;
    printf("%.1f %d\n", selected, a);
    return 0;
}
