#include <stdio.h>

#define VALUE 7
#define APPLY(operation, left, right) operation(left, right)
#define ADD(left, right) ((left) + (right))

#if !defined(MISSING_SYMBOL) && ((VALUE << 1) == 14)
#define FIRST_RESULT 1
#else
#define FIRST_RESULT 0
#endif

#undef VALUE
#define VALUE 9

int main(void)
{
    printf("%d %d %d\n",
        FIRST_RESULT,
        VALUE,
        APPLY(ADD, 4, 5));
    return 0;
}
