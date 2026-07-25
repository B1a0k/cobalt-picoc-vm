int recurse(int value)
{
    return recurse(value + 1);
}

int main(void)
{
    return recurse(0);
}
