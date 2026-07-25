struct Pair {
    int left;
    int right;
};

int main(void)
{
    struct Pair value = {.right = 7, .left = 3};
    return value.left + value.right;
}
