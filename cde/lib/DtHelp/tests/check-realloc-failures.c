/*
 * Behavioural tests for the growth of buffers in DtHelp when realloc() fails.
 *
 * The runner (check-realloc-failures.py) compiles one production source file
 * per case, with realloc-inject.h in front of it and this file appended, so
 * that the functions below are the production ones, compiled together with
 * the static functions they use.  Exactly one CASE_xxx macro is defined per
 * compilation.  The wrappers make the Nth realloc() fail with the old block
 * still valid, and count the blocks that are still allocated.
 */

#if defined(CASE_LINKMGR)
/* GetNextEntry() through _DtLinkDbAddLink(): a failed growth keeps the list. */
int _DtCvRunInterp(int (*filter_exec)(_DtCvPointer, const char *, char **),
		   _DtCvPointer client_data, char *interp, char *data,
		   char **ret_data)
{
    return -1;
}

int main(void)
{
    _DtCvLinkDb db;
    int i;

    db = _DtLinkDbCreate();
    assert(db != nullptr);

    ck_arm(0);
    for (i = 0; i < GROW_SIZE; i++)
	assert(_DtLinkDbAddLink(db, nullptr, "spec", 0, 0, nullptr) == i);
    assert(db->max == GROW_SIZE);

    /* The list is full and cannot grow: the call fails, the list stays. */
    ck_arm(1);
    assert(_DtLinkDbAddLink(db, nullptr, "spec", 0, 0, nullptr) == -1);
    assert(ck_realloc_calls == 1);
    assert(db->list != nullptr);
    assert(db->max == GROW_SIZE);
    for (i = 0; i < db->max; i++)
	assert(db->list[i].type == _DtLinkTypeLink);

    /* Once memory is available again the list grows and is usable. */
    ck_arm(0);
    assert(_DtLinkDbAddLink(db, nullptr, "more", 0, 0, nullptr) == GROW_SIZE);
    assert(db->max == 2 * GROW_SIZE);

    _DtLinkDbDestroy(db);
    assert(ck_live == 0);
    puts("LinkMgr: a failed list growth keeps the list and its size");
    return 0;
}

#elif defined(CASE_UTILSDL)
/* _DtHelpCeGetSdlAttributeCdata(): a failed growth releases the string. */
static int no_more_data(BufFilePtr f)
{
    return BUFFILEEOF;
}

int main(void)
{
    static BufFileRec file;
    char text[130];
    char *str = nullptr;
    int live;

    /* a quoted value of 100 characters needs one growth beyond 64 */
    text[0] = '"';
    memset(text + 1, 'a', 100);
    text[101] = '"';
    text[102] = '\0';

    file.bufp = (BufChar *) text;
    file.left = 102;
    file.read = no_more_data;

    /* control: the value is read */
    ck_arm(0);
    live = ck_live;
    assert(_DtHelpCeGetSdlAttributeCdata(&file, False, &str) == 0);
    assert(ck_realloc_calls == 1);
    assert(str != nullptr && strlen(str) == 100);
    free(str);
    assert(ck_live == live);

    /* the growth fails: the call fails and nothing is left allocated */
    file.bufp = (BufChar *) text;
    file.left = 102;
    str = nullptr;
    ck_arm(1);
    assert(_DtHelpCeGetSdlAttributeCdata(&file, False, &str) == -1);
    assert(ck_realloc_calls == 1);
    assert(str == nullptr);
    assert(ck_live == live);
    puts("UtilSDL: a failed string growth fails the call and frees the string");
    return 0;
}

#elif defined(CASE_CANVASOS)
/* _DtCvRunInterp(): a failed growth of the output keeps nothing allocated. */

/*
 * The production _DtHelpCeGetNxtBuf() copies the buffer onto itself with
 * strcpy(), which the sanitizer rejects; this one reads the same way.
 */
int _DtHelpCeGetNxtBuf(BufFilePtr file, char *dst, char **src, int max_size)
{
    int leftOver = strlen(*src);
    int result;

    memmove(dst, *src, leftOver + 1);
    result = _DtHelpCeBufFileRd(file, &dst[leftOver], max_size - leftOver - 1);
    if (result != -1) {
	dst[leftOver + result] = '\0';
	*src = dst;
    }
    return result;
}

int main(void)
{
    char *out = nullptr;
    int r;

    ck_arm(0);
    r = _DtCvRunInterp(nullptr, nullptr, "/bin/sh", "yes x | head -n 6000\n", &out);
    assert(r == 0);
    assert(out != nullptr && strlen(out) == 12000);
    assert(ck_realloc_calls >= 1);
    free(out);
    assert(ck_live == 0);

    /* the first growth of the output fails */
    out = nullptr;
    ck_arm(1);
    r = _DtCvRunInterp(nullptr, nullptr, "/bin/sh", "yes x | head -n 6000\n", &out);
    assert(r == -1);
    assert(out == nullptr);
    assert(ck_live == 0);
    puts("CanvasOs: a failed output growth fails the call and frees the output");
    return 0;
}

#elif defined(CASE_STRINGFUNCS)
/* _DtHelpCeIconvStr(): failed growth of the converted string. */
int main(void)
{
    _DtHelpCeIconvContext ctx;
    char latin1[30];
    char *out, *keep;
    size_t outLen;
    int r, grown;

    memset(latin1, 0xe9, 29);	/* 29 accented letters, 2 bytes each in UTF-8 */
    latin1[29] = '\0';

    /* control: the string is converted in several growth steps */
    assert(_DtHelpCeIconvOpen(&ctx, "ISO-8859-1", "UTF-8", 0, 0) == 0);
    ck_arm(0);
    out = nullptr;
    assert(_DtHelpCeIconvStr(ctx, latin1, &out, nullptr, nullptr, 0) == 0);
    assert(out != nullptr && strlen(out) == 58);
    grown = ck_realloc_calls;	/* the first block is malloc'd, the rest are growths */
    assert(grown >= 2);
    free(out);
    assert(ck_live == 0);

    /* the last growth fails: the string this routine allocated is freed */
    ck_arm(grown);
    out = (char *) 1;
    assert(_DtHelpCeIconvStr(ctx, latin1, &out, nullptr, nullptr, 0) == -3);
    assert(out == nullptr);
    assert(ck_live == 0);

    /* the caller's buffer is handed back, still valid and not freed */
    keep = malloc(4);
    assert(keep != nullptr);
    ck_arm(1);
    out = keep;
    outLen = 4;
    r = _DtHelpCeIconvStr(ctx, latin1, &out, &outLen, keep, 4);
    assert(r == -3);
    assert(out == keep);
    assert(outLen == 4);
    assert(keep[0] == '\0');
    free(keep);
    assert(ck_live == 0);
    _DtHelpCeIconvClose(&ctx);

    /* same codesets: a plain copy into the caller's buffer */
    assert(_DtHelpCeIconvOpen(&ctx, "UTF-8", "UTF-8", 0, 0) == 0);
    keep = malloc(2);
    assert(keep != nullptr);
    ck_arm(0);
    out = keep;
    outLen = 2;
    assert(_DtHelpCeIconvStr(ctx, "hello world", &out, &outLen, keep, 2) == 0);
    assert(strcmp(out, "hello world") == 0 && outLen == 12);
    free(out);

    keep = malloc(2);
    assert(keep != nullptr);
    ck_arm(1);
    out = (char *) 1;
    outLen = 0;
    r = _DtHelpCeIconvStr(ctx, "hello world", &out, &outLen, keep, 2);
    assert(r == -3);
    assert(out == keep);
    free(keep);
    assert(ck_live == 0);
    _DtHelpCeIconvClose(&ctx);
    puts("StringFuncs: a failed growth never leaves the caller without its buffer");
    return 0;
}

#elif defined(CASE_ACCESSSDL)
/* AsciiKeyword(): a failed growth releases the string built so far. */
int main(void)
{
    _DtCvSegment first, second;
    int size = 0;
    char *str;

    memset(&first, 0, sizeof(first));
    memset(&second, 0, sizeof(second));
    first.type = second.type = _DtCvSTRING;
    first.handle.string.string = "ab";
    second.handle.string.string = "cd";
    first.next_seg = &second;

    ck_arm(0);
    str = AsciiKeyword(&first, nullptr, &size);
    assert(str != nullptr && strcmp(str, "abcd") == 0);
    assert(ck_realloc_calls == 1);
    free(str);
    assert(ck_live == 0);

    size = 0;
    ck_arm(1);
    str = AsciiKeyword(&first, nullptr, &size);
    assert(str == nullptr);
    assert(ck_live == 0);
    puts("AccessSDL: a failed keyword growth fails the call and frees the string");
    return 0;
}

#elif defined(CASE_FORMATSDL_STRING)
/* CreateAsciiString(): a failed growth keeps the string built so far. */

/* The routine returned void before it could report a failed growth. */
#define CREATE_ASCII_STRING(...) \
    __builtin_choose_expr( \
	__builtin_types_compatible_p(__typeof__(CreateAsciiString(__VA_ARGS__)), void), \
	(CreateAsciiString(__VA_ARGS__), 0), CreateAsciiString(__VA_ARGS__))

int main(void)
{
    _DtCvSegment first, second;
    _DtCvValue nl = False;
    char *str = nullptr;

    memset(&first, 0, sizeof(first));
    memset(&second, 0, sizeof(second));
    first.type = second.type = _DtCvSTRING;
    first.handle.string.string = "ab";
    second.handle.string.string = "cd";
    first.next_seg = &second;

    /* control */
    ck_arm(0);
    assert(CREATE_ASCII_STRING(&first, nullptr, &nl, &str) == 0);
    assert(str != nullptr && strcmp(str, "ab cd") == 0);
    assert(ck_realloc_calls == 1);
    free(str);
    assert(ck_live == 0);

    /* the second string cannot be appended: the first one is kept */
    str = nullptr;
    nl = False;
    ck_arm(1);
    assert(CREATE_ASCII_STRING(&first, nullptr, &nl, &str) == -1);
    assert(str != nullptr && strcmp(str, "ab") == 0);
    free(str);
    assert(ck_live == 0);
    puts("FormatSDL: a failed string growth keeps the string built so far");
    return 0;
}

#elif defined(CASE_FORMATSDL_CHUNKS)
/* CreateTitleChunks(): a failed growth keeps the list built so far. */
int main(void)
{
    _DtCvSegment first, second;
    void **chunks = nullptr;
    int cnt = 0;

    memset(&first, 0, sizeof(first));
    memset(&second, 0, sizeof(second));
    first.type = second.type = _DtCvSTRING;
    first.handle.string.string = "ab";
    second.handle.string.string = "cd";
    first.next_seg = &second;

    /* control: both strings become chunks */
    ck_arm(0);
    assert(CreateTitleChunks(nullptr, &first, nullptr, nullptr, "C", "ISO-8859-1",
			     nullptr, nullptr, &cnt, &chunks) == 0);
    assert(cnt == 7 && chunks != nullptr);
    free(chunks[2]);
    free(chunks[5]);
    free(chunks);
    assert(ck_live == 0);

    /* the list cannot grow for the second string: the first one is kept */
    chunks = nullptr;
    cnt = 0;
    ck_arm(1);
    assert(CreateTitleChunks(nullptr, &first, nullptr, nullptr, "C", "ISO-8859-1",
			     nullptr, nullptr, &cnt, &chunks) == -1);
    assert(chunks != nullptr);
    assert(cnt == 4);
    assert(chunks[3] == (void *) DT_HELP_CE_END);
    free(chunks[2]);
    free(chunks);
    assert(ck_live == 0);
    puts("FormatSDL: a failed chunk list growth keeps the list built so far");
    return 0;
}

#else
#error "define one CASE_xxx macro"
#endif
