int add_one(int value)
{
    return value + 1;
}

int main(void)
{
    int (*function)(int) = &add_one;
    return function(4);
}
