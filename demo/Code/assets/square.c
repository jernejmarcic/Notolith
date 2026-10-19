/* square.c: a C file for the runnable file embed demo; compiled, then run */
#include <stdio.h>

int main(void)
{
    for (int n = 1; n <= 4; ++n)
        printf("%d squared is %d\n", n, n * n);
    return 0;
}
