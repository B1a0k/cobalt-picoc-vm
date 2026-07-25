#include <stdio.h>

int is_odd(int value);

int is_even(int value)
{
    if (value == 0)
        return 1;
    return is_odd(value - 1);
}

int is_odd(int value)
{
    if (value == 0)
        return 0;
    return is_even(value - 1);
}

long long sum_eight(
    int a, int b, int c, int d,
    int e, int f, int g, int h)
{
    return a + b + c + d + e + f + g + h;
}

char narrow(int value)
{
    return value;
}

int persistent_counter(void)
{
    static int count = 40;
    return ++count;
}

int main(void)
{
    int first_count;
    int second_count;
    printf("%d %d\n", is_even(10), is_odd(9));
    printf("%lld\n", sum_eight(1, 2, 3, 4, 5, 6, 7, 8));
    printf("%d\n", narrow(0x123));
    first_count = persistent_counter();
    second_count = persistent_counter();
    printf("%d %d\n", first_count, second_count);
    return 0;
}
