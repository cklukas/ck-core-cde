/*
 * Query a DtSearch database through the DtSrAPI and check the documents that
 * come back.
 *
 *   dtsearch-api-test OCF-FILE QUERY-FILE
 *
 * Each line of QUERY-FILE is "TYPE<TAB>QUERY<TAB>KEY KEY ...": the search type
 * (P, S or W), the query, and the record keys of the documents the query must
 * return, in any order, or "!damaged" for a query on a damaged index, which
 * must end with an error and a message that says so.  The databases are those
 * the ocf file names.  The
 * program exits 0 if every query returned exactly the documents it should,
 * 1 if one did not, and 2 if the engine could not be started.
 */
#include <Dt/Search.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int
compare_keys(const void *a, const void *b)
{
    return strcmp(*(char *const *) a, *(char *const *) b);
}

/* Split a blank separated list into a sorted array of strings, in place. */
static int
split_keys(char *list, char **keys, int max)
{
    int count = 0;
    char *key = strtok(list, " \n");

    while (key != NULL && count < max) {
	keys[count++] = key;
	key = strtok(NULL, " \n");
    }
    qsort(keys, (size_t) count, sizeof(keys[0]), compare_keys);
    return count;
}

static int
check_query(char *dbname, char type, char *query, char *expected_list)
{
    enum { MAX_KEYS = 4096 };
    static char *expected[MAX_KEYS];
    static char *found[MAX_KEYS];
    static char stems[DtSrMAX_STEMCOUNT * DtSrMAXWIDTH_HWORD + 8];
    DtSrResult *results = NULL, *result;
    long resultscount = 0;
    int stemcount = 0;
    int expected_count = split_keys(expected_list, expected, MAX_KEYS);
    int found_count = 0;
    int rc, i;
    int must_fail = (expected_list[0] == '!');

    rc = DtSearchQuery(query, dbname, type, NULL, NULL, &results,
		       &resultscount, stems, &stemcount);
    if (must_fail) {
	/* a damaged index: the search ends with an error and says why */
	const char *messages = DtSearchGetMessages();

	if ((rc == DtSrOK || rc == DtSrNOTAVAIL) ||
	    messages == NULL || strstr(messages, "damaged") == NULL) {
	    fprintf(stderr, "FAIL %c '%s': the search returned %d: %s\n", type,
		    query, rc, messages ? messages : "(no message)");
	    DtSearchFreeMessages();
	    DtSearchFreeResults(&results);
	    return 1;
	}
	DtSearchFreeMessages();
	DtSearchFreeResults(&results);
	printf("ok   %c %s (refused: %d)\n", type, query, rc);
	return 0;
    }
    if (rc != DtSrOK && rc != DtSrNOTAVAIL) {
	fprintf(stderr, "FAIL %c '%s': the search returned %d: %s\n", type,
		query, rc, DtSearchGetMessages());
	DtSearchFreeMessages();
	return 1;
    }
    for (result = results; result != NULL && found_count < MAX_KEYS;
	 result = result->link)
	found[found_count++] = result->reckey;
    qsort(found, (size_t) found_count, sizeof(found[0]), compare_keys);

    if ((long) found_count != resultscount && rc == DtSrOK)
	fprintf(stderr, "note %c '%s': the count says %ld, the list has %d\n",
		type, query, resultscount, found_count);
    if (found_count != expected_count) {
	fprintf(stderr, "FAIL %c '%s': %d documents, expected %d\n", type,
		query, found_count, expected_count);
	DtSearchFreeResults(&results);
	return 1;
    }
    for (i = 0; i < found_count; i++) {
	if (strcmp(found[i], expected[i]) != 0) {
	    fprintf(stderr, "FAIL %c '%s': document %s where %s was expected\n",
		    type, query, found[i], expected[i]);
	    DtSearchFreeResults(&results);
	    return 1;
	}
    }
    DtSearchFreeResults(&results);
    printf("ok   %c %s (%d)\n", type, query, found_count);
    return 0;
}

int
main(int argc, char **argv)
{
    char **dbnames = NULL;
    int dbcount = 0;
    FILE *queries;
    static char line[1 << 17];
    int failures = 0, count = 0;

    if (argc != 3) {
	fprintf(stderr, "usage: %s OCF-FILE QUERY-FILE\n", argv[0]);
	return 2;
    }
    if (DtSearchInit(argv[0], "apitest", DtSrInNOLOCALE, argv[1], NULL,
		     &dbnames, &dbcount) != DtSrOK || dbcount < 1) {
	fprintf(stderr, "cannot start the engine: %s\n",
		DtSearchGetMessages());
	return 2;
    }
    DtSearchSetMaxResults(100000);

    if ((queries = fopen(argv[2], "r")) == NULL) {
	perror(argv[2]);
	return 2;
    }
    while (fgets(line, sizeof(line), queries) != NULL) {
	char *query = strchr(line, '\t');
	char *expected;

	if (query == NULL)
	    continue;
	*query++ = '\0';
	expected = strchr(query, '\t');
	if (expected == NULL)
	    continue;
	*expected++ = '\0';
	failures += check_query(dbnames[0], line[0], query, expected);
	count++;
    }
    fclose(queries);
    printf("%d of %d queries returned the right documents\n", count - failures,
	   count);
    return failures != 0;
}
