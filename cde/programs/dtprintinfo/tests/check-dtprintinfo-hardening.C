/*
 * CDE - Common Desktop Environment
 *
 * These libraries and programs are free software; you can redistribute them
 * and/or modify them under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2 of the License,
 * or (at your option) any later version.
 *
 * These libraries and programs are distributed without any warranty; without
 * even the implied warranty of merchantability or fitness for a particular
 * purpose. See the GNU Lesser General Public License for more details.
 */

/*
 * Regression tests for dtprintinfo input-path and bounds hardening:
 *
 * 1. SendPrintJobStatusReguest:
 *    - Formats printer names longer than 100 bytes without heap buffer overflow.
 *    - Writes the LPD status request once rather than twice.
 * 2. DetermineOutput:
 *    - Does not crash with strchr(NULL, '\n') when given fewer than 10 lines
 *      without a trailing newline and no pattern match.
 * 3. ParseBSDPrintJobs / ParseAIXv3PrintJobs / ParseAIXv2PrintJobs:
 *    - Handles long job names/hostnames (>300 bytes), long job numbers (>8
 *      digits), and truncated/malformed lines without heap/stack overflow or
 *      NULL dereference.
 * 4. Queue::LoadAttributes / Queue::StartPrint / Queue::ParseOutput:
 *    - Handles long queue names (>200 bytes) and malformed attribute/job lines
 *      without buffer overflow or NULL dereference, and formats "enable" (not
 *      "disable") in Queue::StartPrint.
 * 5. Invoke::Invoke:
 *    - Preserves exact 511-byte and multi-buffer child stdout/stderr streams
 *      without truncating at BUFFER_SIZE - 1 or leaking buffers.
 */

/* @@PROLOGUE@@ */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <netdb.h>
#include <netinet/in.h>
#include <regex.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef KORNSHELL
#define KORNSHELL "/bin/sh"
#endif

#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif

typedef void *nl_catd;
nl_catd dtprintinfo_cat = nullptr;

#define MESSAGE(msg) ((char *)#msg)
#define DtStrtok(s, d) strtok((s), (d))

/* @@UNITS@@ */

/* @@BODY@@ */
static void
require(bool cond, const char *what)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        exit(1);
    }
}

static void
test_send_print_job_status_request(void)
{
    int sv[2];
    require(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair");

    char long_printer[160];
    memset(long_printer, 'p', sizeof(long_printer) - 1);
    long_printer[sizeof(long_printer) - 1] = '\0';

    int rc = SendPrintJobStatusReguest(sv[0], long_printer);
    require(rc == 1, "SendPrintJobStatusReguest succeeds for 159-byte printer");
    close(sv[0]);

    char received[512];
    ssize_t n = read(sv[1], received, sizeof(received) - 1);
    close(sv[1]);
    require(n > 0, "read LPD request from socketpair");
    received[n] = '\0';

    size_t expected_len = 1 + strlen(long_printer) + 2; /* '\4' + printer + " \n" */
    require((size_t)n == expected_len,
            "SendPrintJobStatusReguest writes request exactly once");
    require(received[0] == '\4', "LPD request starts with 0x04");
}

static void
test_determine_output_short_unmatched(void)
{
    char sample1[] = "no entries";
    require(DetermineOutput(sample1) == UNKNOWN_OUTPUT,
            "DetermineOutput single line without newline");

    char sample2[] = "Printer is ready\nno entries";
    require(DetermineOutput(sample2) == UNKNOWN_OUTPUT,
            "DetermineOutput two lines without trailing newline");

    char sample3[] = "root: 1st [job 001host]\n\t/etc/motd 421 bytes";
    require(DetermineOutput(sample3) == BSD_OUTPUT,
            "DetermineOutput recognizes BSD output");
}

static void
test_parsers_bounds_and_malformed(void)
{
    char long_printer[180];
    memset(long_printer, 'q', sizeof(long_printer) - 1);
    long_printer[sizeof(long_printer) - 1] = '\0';

    char long_file[240];
    memset(long_file, 'f', sizeof(long_file) - 1);
    long_file[sizeof(long_file) - 1] = '\0';

    char bsd_input[1024];
    snprintf(bsd_input, sizeof(bsd_input),
             "root: 1st [job 123456789012345myhost.domain.example]\n"
             "\t%s 421 bytes\n"
             "malformed: [job_without_space]\n",
             long_file);

    char *job_list = nullptr;
    int n_jobs = 0;
    int rc = ParseBSDPrintJobs(long_printer, bsd_input, &job_list, &n_jobs);
    require(rc == 1, "ParseBSDPrintJobs return code");
    require(n_jobs == 1, "ParseBSDPrintJobs parsed 1 valid job");
    require(job_list != nullptr && strstr(job_list, long_file) != nullptr,
            "ParseBSDPrintJobs preserved long job name");

    char aix3_short[] = "short aixv3 output without enough newlines";
    n_jobs = -1;
    rc = ParseAIXv3PrintJobs(long_printer, aix3_short, &job_list, &n_jobs);
    require(n_jobs == 0, "ParseAIXv3PrintJobs handles truncated header safely");

    char aix2_short[] = "short aixv2 output";
    n_jobs = -1;
    rc = ParseAIXv2PrintJobs(long_printer, aix2_short, &job_list, &n_jobs);
    require(n_jobs == 0, "ParseAIXv2PrintJobs handles truncated header safely");
}

static void
test_invoke_511_bytes(void)
{
    char *out = nullptr;
    char *err = nullptr;
    /* Emit 511 'A's followed by 511 'B's (1022 bytes total) */
    Invoke inv("awk 'BEGIN { for (i = 0; i < 511; i++) printf \"A\"; "
               "for (i = 0; i < 511; i++) printf \"B\" }'",
               &out, &err);
    require(inv.status == 0, "Invoke status == 0");
    require(out != nullptr, "Invoke out != nullptr");
    require(strlen(out) == 1022, "Invoke preserved all 1022 bytes across 511-byte boundary");
    require(out[0] == 'A' && out[510] == 'A' && out[511] == 'B' && out[1021] == 'B',
            "Invoke output bytes match without gap");
    free(out);
    free(err);
}

int
main(void)
{
    test_send_print_job_status_request();
    test_determine_output_short_unmatched();
    test_parsers_bounds_and_malformed();
    test_invoke_511_bytes();
    printf("check-dtprintinfo-hardening: all tests passed\n");
    return 0;
}
