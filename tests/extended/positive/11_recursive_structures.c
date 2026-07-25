#include <stdio.h>

struct Node {
    int value;
    struct Node *next;
};

int main(void)
{
    struct Node first;
    struct Node second;
    struct Node third;
    struct Node *current;
    int total = 0;

    first.value = 10;
    first.next = &second;
    second.value = 20;
    second.next = &third;
    third.value = 30;
    third.next = NULL;

    current = &first;
    while (current != NULL) {
        total += current->value;
        current = current->next;
    }
    printf("%d %d\n", total, sizeof(struct Node));
    return 0;
}
