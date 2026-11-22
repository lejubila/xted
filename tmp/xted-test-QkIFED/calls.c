#include <string.h>
int add(int a, int b)
{
    int total = 0;
    char *buf, *tmp;
    for (int i = 0; i < 3; i++) {
        total += add(i, total);
    }
    strncpy(buf, tmp, 2);
}
int other(int z) { return z; }
static int proto(int p);
int later;
