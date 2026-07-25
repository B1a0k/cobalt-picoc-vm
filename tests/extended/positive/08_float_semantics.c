#include <stdio.h>
#include <math.h>

int main(void)
{
    double negative = -3.75;
    double minus_zero = -0.0;
    float pico_float = 1.0f / 10.0f;
    double selected = 0 ? 7 : 2.25;

    printf("%d %d %d\n",
        (int)negative,
        minus_zero == 0.0,
        minus_zero != 0.0);
    printf("%.12f %.2f\n", pico_float, selected);
    printf("%.3f %.3f\n", sqrt(9.0), fabs(-2.5));
    return 0;
}
