/*
 * Behavioural tests for realloc() failures in DtTerm.
 *
 * The runner (check-realloc-failures.py) puts realloc-inject.h and the
 * definitions named in each case in front of this file, takes the production
 * functions verbatim from the sources, and compiles exactly one CASE_xxx
 * per program.  The wrappers make the Nth realloc() fail with the old block
 * still valid, and count the blocks that are still allocated.
 */

#if defined(CASE_ENUMNAME)
/* enumToName(): a failed growth of the name buffer must not crash. */
int main(void)
{
    enumName names[] = { { "Known", 1 }, { NULL, 0 } };
    char *s;

    ck_arm(0);
    assert(strcmp(enumToName(names, 1), "Known") == 0);
    s = enumToName(names, 42);
    assert(strcmp(s, "Unknown Value 42") == 0);
    assert(ck_realloc_calls == 1);

    /* the buffer cannot be enlarged for the next value */
    ck_arm(1);
    s = enumToName(names, 4711);
    assert(s != NULL);
    assert(strcmp(s, "Unknown Value") == 0);

    /* and works again afterwards */
    ck_arm(0);
    assert(strcmp(enumToName(names, 7), "Unknown Value 7") == 0);
    puts("TermPrimDebug: a failed name buffer growth returns a fixed name");
    return 0;
}

#elif defined(CASE_GETPTY)
/* GetPty(): failed growth of the device name buffers. */
int main(void)
{
    char *slave, *master;
    int fd, call;

    /* nothing can be opened: every failing realloc() releases both names */
    ck_open_succeeds = 0;
    for (call = 1; call <= 4; call++) {
	ck_arm(call);
	slave = master = NULL;
	assert(GetPty(&slave, &master) == -1);
	assert(ck_realloc_calls >= call);
	assert(ck_live == 0);
    }

    puts("TermPrimGetPty: a failed name buffer growth frees both buffers");
    return 0;
}

#elif defined(CASE_GETPTY_TTYNAME)
/* GetPty(): failed growth of the buffer for the name that ttyname() gives. */
int main(void)
{
    char *slave, *master;
    int fd;

    /* control: the name that ttyname() gives replaces the opened one */
    ck_open_succeeds = 1;
    ck_arm(0);
    fd = GetPty(&slave, &master);
    assert(fd >= 0);
    assert(strcmp(slave, CK_TTYNAME) == 0);
    close(fd);
    free(slave);
    free(master);
    assert(ck_live == 0);

    /* the name cannot be stored: the opened name stays in use */
    ck_arm(3);
    fd = GetPty(&slave, &master);
    assert(fd >= 0);
    assert(ck_realloc_calls == 3);
    assert(strncmp(slave, "/dev/pty", 8) == 0);
    assert(strcmp(slave, CK_TTYNAME) != 0);
    close(fd);
    free(slave);
    free(master);
    assert(ck_live == 0);
    puts("TermPrimGetPty: a name that cannot be stored leaves the opened name in use");
    return 0;
}

#else
#error "define one CASE_xxx macro"
#endif
