#include <stdio.h>

typedef void *opaque_t;
typedef int (*VISITOR)(void *context, int values[], int count);
typedef int (*LOG_FN)(char *format, ...);

void no_args(void);

void no_args(void)
{
}

void unspecified_args();

void unspecified_args()
{
}

void release(void *);

void release(void *value)
{
    value = 0;
}

void arrays(int [], void *[]);

void arrays(int values[], void *items[])
{
    values[0] = values[0];
    items[0] = items[0];
}

void assign(void **slot, void *value)
{
    *slot = value;
}

void mixed(int marker, void *value, void **slot)
{
    if (marker)
        assign(slot, value);
}

void *identity(void *value)
{
    return value;
}

opaque_t alias_identity(opaque_t value)
{
    return value;
}

int read_first(void *value)
{
    return ((int *)value)[0];
}

int main(void)
{
    int values[2] = {7, 9};
    void *items[1];
    void *selected = identity(values);
    items[0] = alias_identity(selected);
    mixed(1, values + 1, &selected);
    arrays(values, items);
    release(selected);
    printf(
        "%d %d %d\n",
        read_first(items[0]),
        read_first(selected),
        values[0]);
    return 0;
}
