MSVCRT$_abs64: cdecl i64 (i64);

int main(void)
{
    return (int)MSVCRT$_abs64(-9LL);
}
