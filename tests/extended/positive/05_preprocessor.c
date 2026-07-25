#include <stdio.h>
#include "include/feature_macros.h"

#if defined(NESTED_BASE) && (NESTED_BASE * 2 == 6)
#define CONDITION_RESULT 1
#else
#define CONDITION_RESULT 0
#endif

#define SELECTOR 0
#if SELECTOR
#define BRANCH_RESULT 10
#elif NESTED_BASE == 3
#define BRANCH_RESULT 20
#else
#define BRANCH_RESULT 30
#endif

int main(void)
{
    printf("%d %d %d\n",
        TWICE(NESTED_BASE),
        ADD_LINES(3, 4),
        CONDITION_RESULT);
    printf("%d %s\n", BRANCH_RESULT, "joined " "literal");
    return 0;
}
