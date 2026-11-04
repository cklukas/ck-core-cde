/*
 * Verify that NetISAM and the code that includes isam.h agree on the width of a
 * long.  libisam is built as C and stores LONGSIZE bytes for every long; its
 * clients are C and C++, and they size keys and records with the same macros.
 * The file is built as C and as C++ (check-isam-long-width-cxx.cc) and runs
 * against libisam, so both views are held to what the library does.
 */

#ifdef __cplusplus
extern "C" {
#endif
#include "isam.h"
#ifdef __cplusplus
}
#endif

#include <limits.h>
#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #cond); \
            failures++; \
        } \
    } while (0)

int
main(void)
{
    static const long values[] = {
        0L, 1L, -1L, -2L, 0x7fffffffL, -0x7fffffffL - 1, LONG_MAX, LONG_MIN
    };
    char encoded[32];
    const char *input = encoded;
    size_t stored;
    size_t i;

    CHECK(LONG_BIT == (int) (sizeof(long) * CHAR_BIT));
    CHECK(LONGSIZE == (int) sizeof(long));

    /* the library writes as many bytes as the header says it does */
    memset(encoded, 0x5a, sizeof(encoded));
    CHECK(stlong(1L, encoded) == 0);
    for (stored = 0; stored < sizeof(encoded) && encoded[stored] != 0x5a; stored++)
        ;
    CHECK(stored == (size_t) LONGSIZE);

    /* and gives back what it was given, a negative number included */
    for (i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
        memset(encoded, 0, sizeof(encoded));
        CHECK(stlong(values[i], encoded) == 0);
        CHECK(ldlong(input) == values[i]);
    }

    return failures != 0;
}
