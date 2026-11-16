/*
 * Sanitizer regression harness for dtdocbook, dtudcexch, dtudcfonted,
 * dtexec, and CanvasSegP.h hardening fixes:
 *  - dtudcexch/importbdf.c: impMergePtn previously stored the address of
 *    local stack variable `char *ptn` into `r_udc->ptn = &ptn;` (dangling
 *    stack pointer / autoVariables + memory leak)
 *  - dtudcexch/selectxlfd.c: i2c previously returned uninitialized stack
 *    char when `num < 0 || num > 15` and i2s assigned `nullptr` to `char`
 *  - dtexec/Main.c: CommandListJoin previously accepted negative `list->count`,
 *    calling `malloc(0)` and writing `*ptr = '\0'` out of bounds
 */
#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MALLOC_ERROR -1
#define FILE_END 1

struct btophead {
    int bdf_width;
    int bdf_height;
    int zoomf;
    int num_chars;
    int *code;
    char **ptn;
};

typedef struct {
    char **items;
    int count;
} CommandList;

static void *
XtCalloc(unsigned int num, unsigned int size)
{
    return calloc(num, size);
}

static int
GetBdfCode(struct btophead *r_udc, char *buf, int *code)
{
    (void)r_udc;
    (void)buf;
    *code = 0;
    return FILE_END;
}

static int
expCheckCode(int code, int bdf_codenum, int *bdf_codelist)
{
    (void)code;
    (void)bdf_codenum;
    (void)bdf_codelist;
    return 1;
}

static int
impModifyPtn(struct btophead *r_udc, struct btophead *r_gpf, char *buf, int j)
{
    (void)r_udc;
    (void)r_gpf;
    (void)buf;
    (void)j;
    return 0;
}

static int
impInsertPtn(struct btophead *r_udc, struct btophead *r_gpf, char *buf, int code, int j)
{
    (void)r_udc;
    (void)r_gpf;
    (void)buf;
    (void)code;
    (void)j;
    return 0;
}

/* @@EXTRACTED_FUNCTIONS@@ */

static void
clobber_stack(void)
{
    volatile uintptr_t junk[64];
    for (size_t i = 0; i < 64; i++)
        junk[i] = (uintptr_t)0xdeadbeefcafebabeULL;
    (void)junk;
}

static void
test_imp_merge_ptn_no_dangling_stack(void)
{
    struct btophead udc;
    struct btophead gpf;
    memset(&udc, 0, sizeof(udc));
    memset(&gpf, 0, sizeof(gpf));
    udc.bdf_width = 16;
    udc.bdf_height = 16;
    udc.num_chars = 1;
    gpf.bdf_width = 24;
    gpf.bdf_height = 24;

    char buf[64] = {0};
    assert(impMergePtn(&udc, &gpf, buf, 0, nullptr) == 0);
    assert(udc.zoomf == 1);
    assert(udc.ptn != nullptr);

    /* Clobber stack frames and then write through udc.ptn[0]:
     * previously udc.ptn pointed to local stack variable `ptn` inside impMergePtn!
     */
    clobber_stack();
    memset(udc.ptn[0], 0x5a, ((16 + 7) / 8) * 16);
    free(udc.ptn[0]);
    free(udc.ptn);
}

static void
test_selectxlfd_i2c_and_i2s(void)
{
    assert(i2c(0) == '0');
    assert(i2c(15) == 'f');
    assert(i2c(99) == '\0');
    char *s = i2s(0x2a);
    assert(s != nullptr);
    assert(strcmp(s, "0x2a") == 0);
    free(s);
}

static void
test_dtexec_command_list_join_negative_count(void)
{
    CommandList list = { nullptr, -3 };
    /* Previously allocated malloc(0) and wrote *ptr = '\0' when count < 0 */
    assert(CommandListJoin(&list) == nullptr);
}

int
main(void)
{
    test_imp_merge_ptn_no_dangling_stack();
    test_selectxlfd_i2c_and_i2s();
    test_dtexec_command_list_join_negative_count();
    printf("check-dtdocbook-udc-hardening: all tests passed\n");
    return 0;
}
