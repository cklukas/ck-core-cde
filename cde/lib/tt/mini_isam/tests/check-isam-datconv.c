/*
 * Verify the public NetISAM data conversion interfaces with fully typed
 * declarations.  The encoded representation is intentionally exercised
 * through const input buffers so prototype regressions fail at compile time.
 */

#include "isam.h"

#include <assert.h>
#include <string.h>

int
main(void)
{
    char encoded[16];
    char text[8];
    const char *input;

    memset(encoded, 0, sizeof(encoded));
    assert(stlong(0x12345678L, encoded) == 0);
    input = encoded;
    assert(ldlong(input) == 0x12345678L);

    memset(encoded, 0, sizeof(encoded));
    assert(stint(-1234, encoded) == 0);
    input = encoded;
    assert(ldint(input) == -1234);

    memset(encoded, 0, sizeof(encoded));
    assert(stfloat(1.25F, encoded) == 0);
    input = encoded;
    assert(ldfloat(input) == 1.25F);

    memset(encoded, 0, sizeof(encoded));
    assert(stdbl(3.5, encoded) == 0);
    input = encoded;
    assert(lddbl(input) == 3.5);

    memset(encoded, 0, sizeof(encoded));
    assert(stchar("C23", encoded, 6) == 0);
    input = encoded;
    assert(ldchar(input, 6, text) == 0);
    assert(strcmp(text, "C23") == 0);

    return 0;
}
