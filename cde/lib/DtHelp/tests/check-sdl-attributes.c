/*
 * Behavioural tests for the attribute value readers of the SDL token layer
 * (UtilSDL.c): _DtHelpCeGetSdlNumber, _DtHelpCeGetSdlId and
 * _DtHelpCeGetSdlAttributeCdata.
 *
 * The runner (check-sdl-attributes.py) compiles UtilSDL.c with
 * realloc-inject.h in front of it and this file appended, so the functions
 * below are the production ones.  The wrappers of realloc-inject.h count the
 * blocks that are still allocated; free() of a pointer that is not a heap
 * block is an error of the address sanitizer.  The scenario to run is the
 * first argument:
 *
 *   number  a number is read into the caller's stack buffer, as
 *           ProcessSDLAttribute() does: a value cut off by the end of the
 *           file, a value without quote and one that is too long fail, and
 *           the buffer is never passed to free()
 *   id      an empty or malformed id fails without a null dereference and
 *           without leaking
 *   valid   well-formed attributes parse as before
 */

static int no_more_data(BufFilePtr f)
{
    f->left = 0;
    return BUFFILEEOF;
}

static BufFileRec file;
static char text[256];

static void feed(const char *s)
{
    strcpy(text, s);
    file.bufp = (BufChar *) text;
    file.left = strlen(text);
    file.read = no_more_data;
}

#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

static void number_scenario(void)
{
    char numBuf[DTD_NAMELEN + 1];
    char big[DTD_NAMELEN + 8];
    int live = ck_live;

    memset(numBuf, 'x', sizeof numBuf);

    /* a value cut off by the end of the file */
    feed("\"123");
    CHECK(_DtHelpCeGetSdlNumber(&file, numBuf) == -1);
    feed("\"");
    CHECK(_DtHelpCeGetSdlNumber(&file, numBuf) == -1);
    feed("");
    CHECK(_DtHelpCeGetSdlNumber(&file, numBuf) == -1);
    feed("   '7");
    CHECK(_DtHelpCeGetSdlNumber(&file, numBuf) == -1);

    /* no opening quote */
    feed("123\"");
    CHECK(_DtHelpCeGetSdlNumber(&file, numBuf) == -1);

    /* longer than the buffer: DTD_NAMELEN digits fit, one more does not */
    memset(big, '5', sizeof big);
    big[0] = '"';
    big[DTD_NAMELEN + 1] = '"';
    big[DTD_NAMELEN + 2] = '\0';
    feed(big);
    CHECK(_DtHelpCeGetSdlNumber(&file, numBuf) == 0);
    CHECK(strlen(numBuf) == DTD_NAMELEN);
    big[DTD_NAMELEN + 1] = '5';
    big[DTD_NAMELEN + 2] = '"';
    big[DTD_NAMELEN + 3] = '\0';
    feed(big);
    CHECK(_DtHelpCeGetSdlNumber(&file, numBuf) == -1);

    /* the limited reader on its own, cut off */
    {
	char *p = numBuf;

	feed("\"12");
	CHECK(_DtHelpCeGetSdlAttributeCdata(&file, True, &p) == -1);
	feed("x");
	CHECK(_DtHelpCeGetSdlAttributeCdata(&file, True, &p) == -1);
    }

    /* not a number, empty */
    feed("\"abc\"");
    CHECK(_DtHelpCeGetSdlNumber(&file, numBuf) == -1);
    feed("\"\"");
    CHECK(_DtHelpCeGetSdlNumber(&file, numBuf) == -1);

    CHECK(ck_live == live);
    puts("UtilSDL: a number that fails to read never frees the caller's buffer");
}

static void id_scenario(void)
{
    char *id = (char *) 1;
    int live = ck_live;

    feed("\"\"");
    CHECK(_DtHelpCeGetSdlId(&file, &id) == -1);
    CHECK(id == nullptr);
    feed("''");
    CHECK(_DtHelpCeGetSdlId(&file, &id) == -1);
    CHECK(id == nullptr);

    /* not a name: nothing stays allocated */
    feed("\"1abc\"");
    CHECK(_DtHelpCeGetSdlId(&file, &id) == -1);
    CHECK(id == nullptr);
    CHECK(ck_live == live);

    /* cut off, no quote */
    feed("\"abc");
    CHECK(_DtHelpCeGetSdlId(&file, &id) == -1);
    CHECK(ck_live == live);
    feed("abc\"");
    CHECK(_DtHelpCeGetSdlId(&file, &id) == -1);
    CHECK(ck_live == live);
    feed("");
    CHECK(_DtHelpCeGetSdlId(&file, &id) == -1);
    CHECK(ck_live == live);

    puts("UtilSDL: an empty or malformed id fails cleanly");
}

static void valid_scenario(void)
{
    char numBuf[DTD_NAMELEN + 1];
    char *str = nullptr;
    int live = ck_live;

    feed("\"42\"");
    CHECK(_DtHelpCeGetSdlNumber(&file, numBuf) == 0);
    CHECK(strcmp(numBuf, "42") == 0);
    feed(" \n\t'007' rest");
    CHECK(_DtHelpCeGetSdlNumber(&file, numBuf) == 0);
    CHECK(strcmp(numBuf, "007") == 0);

    feed("\"abc-1\"");
    CHECK(_DtHelpCeGetSdlId(&file, &str) == 0);
    CHECK(strcmp(str, "abc-1") == 0);
    free(str);
    CHECK(ck_live == live);

    /* a newline in a value becomes a space, the other quote is data */
    feed("'a\nb\"c'");
    str = nullptr;
    CHECK(_DtHelpCeGetSdlAttributeCdata(&file, False, &str) == 0);
    CHECK(strcmp(str, "a b\"c") == 0);
    free(str);

    /* an empty value gives no string, as before */
    feed("\"\"");
    str = (char *) 1;
    CHECK(_DtHelpCeGetSdlAttributeCdata(&file, False, &str) == 0);
    CHECK(str == nullptr);

    /* the value is skipped: no string is wanted */
    feed("\"skipped\"");
    CHECK(_DtHelpCeGetSdlAttributeCdata(&file, False, nullptr) == 0);
    feed("\"cut off");
    CHECK(_DtHelpCeGetSdlAttributeCdata(&file, False, nullptr) == -1);

    /* a cut off unlimited value releases what it read */
    feed("\"abc");
    str = nullptr;
    CHECK(_DtHelpCeGetSdlAttributeCdata(&file, False, &str) == -1);
    CHECK(ck_live == live);

    puts("UtilSDL: well-formed attribute values parse as before");
}

int main(int argc, char **argv)
{
    if (argc != 2)
	return 2;
    if (strcmp(argv[1], "number") == 0)
	number_scenario();
    else if (strcmp(argv[1], "id") == 0)
	id_scenario();
    else if (strcmp(argv[1], "valid") == 0)
	valid_scenario();
    else
	return 2;
    return 0;
}
