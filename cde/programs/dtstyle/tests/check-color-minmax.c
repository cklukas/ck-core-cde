/* Harness for check-color-minmax.py: max() and min() of ColorEdit.c must
 * return a value for every input, including NaN, where every comparison
 * is false. */
/* @@PROLOGUE@@ */
#define _GNU_SOURCE
#include <stdio.h>
#include <math.h>
#include <assert.h>
/* @@BODY@@ */
int main(void)
{
    volatile double nan = NAN;

    assert(max(1.0, 2.0, 3.0) == 3.0);
    assert(max(3.0, 2.0, 1.0) == 3.0);
    assert(max(2.0, 3.0, 1.0) == 3.0);
    assert(min(3.0, 2.0, 1.0) == 1.0);
    assert(min(1.0, 2.0, 3.0) == 1.0);
    assert(min(2.0, 1.0, 3.0) == 1.0);
    /* with NaN no comparison holds; the result must still be defined */
    assert(max(nan, 1.0, 2.0) == 2.0);
    assert(min(nan, 1.0, 2.0) == 2.0);
    puts("color min/max ok");
    return 0;
}
