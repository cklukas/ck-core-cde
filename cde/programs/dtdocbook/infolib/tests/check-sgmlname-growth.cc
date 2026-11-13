/* Driver for check-sgmlname-growth.py: interns enough names to make the name
   table grow several times, then looks every name up again. */
#include <stdio.h>
#include <string.h>

#include "SGMLName.h"

int main()
{
    char name[32];
    int codes[300];

    for (int i = 0; i < 300; i++) {
        snprintf(name, sizeof name, "name-%d", i);
        codes[i] = SGMLName::intern(name, 1);
    }
    for (int i = 0; i < 300; i++) {
        snprintf(name, sizeof name, "NAME-%d", i);
        if (SGMLName::intern(name) != codes[i] || strcmp(SGMLName::lookup(codes[i]), name) != 0) {
            fprintf(stderr, "name %d changed its code\n", i);
            return 1;
        }
    }
    return 0;
}
