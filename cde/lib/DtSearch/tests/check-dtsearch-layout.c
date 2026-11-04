/*
 * Verify that the headers of a DtSearch database give C and C++ the same
 * widths: INT is 2 bytes, DtSrINT32 is 4 bytes, and LONG, ULONG, DB_ADDR,
 * F_ADDR and DtSrINT64 are 8 bytes, whatever feature macros the build has set
 * and whether LONG_BIT is defined.  The file is built as C (with the layout
 * constants of the data dictionary, which are C only) and as C++
 * (check-dtsearch-layout-cxx.cc), the way the library and its C++ users
 * (dtinfo) see the headers.  The byte order of an address in a file is
 * big-endian.
 */
#include "Search.h"
#include "vista.h"
#ifndef __cplusplus
#include "dbtype.h"
#endif

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
    unsigned char bytes[sizeof(DB_ADDR)];
    DB_ADDR addr = 0x0102030405060708LL;
    ULONG stamp = 0x1112131415161718ULL;
    DB_ADDR back;

    CHECK(sizeof(INT) == 2);
    CHECK(sizeof(UINT) == 2);
    CHECK(sizeof(LONG) == 8);
    CHECK(sizeof(ULONG) == 8);
    CHECK(sizeof(DB_ADDR) == 8);
    CHECK(sizeof(F_ADDR) == 8);
    CHECK(sizeof(DtSrINT16) == 2);
    CHECK(sizeof(DtSrINT32) == 4);
    CHECK(sizeof(DtSrUINT32) == 4);
    CHECK(sizeof(DtSrINT64) == 8);
    CHECK(sizeof(DtSrObjdate) == 4);

    /* an address and a timestamp go into a file big-endian, whole */
    addr = (DB_ADDR) dt_net64((uint64_t) addr);
    memcpy(bytes, &addr, sizeof(bytes));
    CHECK(bytes[0] == 1 && bytes[7] == 8);
    back = (DB_ADDR) dt_net64((uint64_t) addr);
    CHECK(back == 0x0102030405060708LL);
    stamp = dt_net64(stamp);
    memcpy(bytes, &stamp, sizeof(bytes));
    CHECK(bytes[0] == 0x11 && bytes[7] == 0x18);
    addr = -1;
    CHECK((DB_ADDR) dt_net64((uint64_t) addr) == -1);

#ifndef __cplusplus
    /* the layout of the slots of a database file */
    CHECK(DB_ADDR_SIZE == 8);
    CHECK(PGHDRSIZE == 8);
    CHECK(RECHDRSIZE == 10);
    CHECK(RECUPTIME == RECCRTIME + 8);
    CHECK(SETPSIZE == 32);
    CHECK(MEMPSIZE == 24);
    CHECK(SP_FIRST == 8 && SP_LAST == 16);
    CHECK(MP_PREV == 8 && MP_NEXT == 16);
#endif

    return failures != 0;
}
