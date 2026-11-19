#define _DEFAULT_SOURCE
#include "padFeatures.h"

#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void)
{
    char cwd[PATH_MAX];
    assert(getcwd(cwd, sizeof(cwd)) != NULL);

    char recent_store[PATH_MAX];
    char autosave_dir[PATH_MAX];
    snprintf(recent_store, sizeof(recent_store), "%s/dtpad.recent", cwd);
    snprintf(autosave_dir, sizeof(autosave_dir), "%s/autosave", cwd);

    /* 1. Recent files MRU ordering + deduplication + cap at DTPAD_MAX_RECENT */
    char f1[PATH_MAX], f2[PATH_MAX], f3[PATH_MAX];
    snprintf(f1, sizeof(f1), "%s/alpha.txt", cwd);
    snprintf(f2, sizeof(f2), "%s/beta.txt", cwd);
    snprintf(f3, sizeof(f3), "%s/gamma.txt", cwd);

    FILE *fp = fopen(f1, "w"); assert(fp); fprintf(fp, "one\n"); fclose(fp);
    fp = fopen(f2, "w"); assert(fp); fprintf(fp, "two\n"); fclose(fp);
    fp = fopen(f3, "w"); assert(fp); fprintf(fp, "three\n"); fclose(fp);

    assert(dtpad_recent_record(recent_store, f1) == 0);
    assert(dtpad_recent_record(recent_store, f2) == 0);
    assert(dtpad_recent_record(recent_store, f3) == 0);
    /* Re-opening alpha moves it to top without duplicating */
    assert(dtpad_recent_record(recent_store, f1) == 0);

    char loaded[DTPAD_MAX_RECENT][PATH_MAX];
    int cnt = dtpad_recent_load(recent_store, loaded);
    assert(cnt == 3);
    assert(strstr(loaded[0], "alpha.txt") != NULL);
    assert(strstr(loaded[1], "gamma.txt") != NULL);
    assert(strstr(loaded[2], "beta.txt") != NULL);

    /* 2. Named-file crash recovery from dead child PID */
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        char swap_out[PATH_MAX];
        const char *unsaved = "Recovered line 1\nRecovered line 2\n";
        int rc = dtpad_autosave_write(autosave_dir, f2, (int)getpid(), 1,
                                      unsaved, strlen(unsaved), swap_out, sizeof(swap_out));
        _exit(rc == 0 ? 0 : 2);
    }
    int status = 0;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);

    char *rec_body = NULL;
    size_t rec_len = 0;
    char rec_swap[PATH_MAX] = "";
    assert(dtpad_autosave_find_recovery(autosave_dir, f2, &rec_body, &rec_len,
                                        rec_swap, sizeof(rec_swap)) == 0);
    assert(rec_body != NULL);
    assert(strcmp(rec_body, "Recovered line 1\nRecovered line 2\n") == 0);
    free(rec_body);

    /* Normal save removes autosave swap file */
    assert(dtpad_autosave_remove_for_path(autosave_dir, f2) == 0);
    assert(dtpad_autosave_find_recovery(autosave_dir, f2, &rec_body, &rec_len,
                                        rec_swap, sizeof(rec_swap)) == -1);

    /* 3. Untitled crash recovery from dead child PID */
    pid_t child2 = fork();
    assert(child2 >= 0);
    if (child2 == 0) {
        char swap_out[PATH_MAX];
        const char *scratch = "Unsaved scratch notes before SIGKILL";
        (void)dtpad_autosave_write(autosave_dir, NULL, (int)getpid(), 1,
                                   scratch, strlen(scratch), swap_out, sizeof(swap_out));
        raise(SIGKILL);
        _exit(1);
    }
    assert(waitpid(child2, &status, 0) == child2);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);

    rec_body = NULL;
    rec_len = 0;
    assert(dtpad_autosave_find_recovery(autosave_dir, NULL, &rec_body, &rec_len,
                                        rec_swap, sizeof(rec_swap)) == 0);
    assert(rec_body != NULL);
    assert(strcmp(rec_body, "Unsaved scratch notes before SIGKILL") == 0);
    free(rec_body);
    assert(dtpad_autosave_remove_file(rec_swap) == 0);

    printf("pad-features-test: all checks passed!\n");
    return 0;
}
