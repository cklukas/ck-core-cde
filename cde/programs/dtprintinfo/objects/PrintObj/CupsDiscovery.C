/*
 * CDE - Common Desktop Environment
 *
 * CupsDiscovery.C: Driverless CUPS / IPP Everywhere printer & print-job
 * discovery via libcups.so.2 (cupsGetDests2 / cupsGetJobs2 / cupsCancelJob2)
 * with $CK_CUPS_MOCK_FILE override support for headless qualification.
 */

#include "CupsDiscovery.h"

#include <ctype.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

/* Stable libcups.so.2 ABI structures (CUPS 1.2 - 2.4+) */
typedef struct {
    char *name;
    char *value;
} LocalCupsOption;

typedef struct {
    char *name;
    char *instance;
    int   is_default;
    int   num_options;
    LocalCupsOption *options;
} LocalCupsDest;

typedef struct {
    int    id;
    char  *dest;
    char  *title;
    char  *user;
    char  *format;
    int    state; /* 3=pending, 4=held, 5=processing, 6=stopped, 7=canceled, 8=aborted, 9=completed */
    int    size;
    int    priority;
    time_t completed_time;
    time_t creation_time;
    time_t processing_time;
} LocalCupsJob;

static void safe_str_copy(char *dst, size_t cap, const char *src) {
    if (!dst || cap == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static bool detect_ipp_everywhere(const char *uri, const char *make_model, const char *type_opt) {
    if (uri && (strncasecmp(uri, "ipp://", 6) == 0 ||
                strncasecmp(uri, "ipps://", 7) == 0 ||
                strncasecmp(uri, "dnssd://", 8) == 0 ||
                strncasecmp(uri, "implicitclass://", 16) == 0)) {
        return true;
    }
    if (make_model && (strcasestr(make_model, "IPP Everywhere") != NULL ||
                       strcasestr(make_model, "driverless") != NULL ||
                       strcasestr(make_model, "AirPrint") != NULL)) {
        return true;
    }
    if (type_opt) {
        unsigned long flags = strtoul(type_opt, NULL, 0);
        /* CUPS_PRINTER_DISCOVERED = 0x1000000 */
        if (flags & 0x1000000UL) return true;
    }
    return false;
}

static const char *ipp_pstate_str(const char *state_opt) {
    if (!state_opt) return "idle";
    int v = atoi(state_opt);
    if (v == 4 || strcasecmp(state_opt, "processing") == 0) return "processing";
    if (v == 5 || strcasecmp(state_opt, "stopped") == 0) return "stopped";
    return "idle";
}

static const char *ipp_jstate_str(int state) {
    switch (state) {
    case 3: return "pending";
    case 4: return "held";
    case 5: return "processing";
    case 6: return "stopped";
    case 7: return "canceled";
    case 8: return "aborted";
    case 9: return "completed";
    default: return "pending";
    }
}

static void format_epoch_date_time(time_t t, char *out_date, size_t dcap, char *out_time, size_t tcap) {
    if (t <= 0) t = time(NULL);
    struct tm ltm;
    if (localtime_r(&t, &ltm)) {
        strftime(out_date, dcap, "%Y-%m-%d", &ltm);
        strftime(out_time, tcap, "%H:%M:%S", &ltm);
    } else {
        safe_str_copy(out_date, dcap, "Today");
        safe_str_copy(out_time, tcap, "00:00:00");
    }
}

/* Parse $CK_CUPS_MOCK_FILE supporting pipe-delimited records:
 *   PRINTER|<name>|<uri>|<make_model>|<location>|<state>|<accepting 0|1>|<default 0|1>|<ipp_everywhere 0|1>
 *   JOB|<id>|<dest>|<title>|<user>|<state>|<size_kb>|<date>|<time>
 */
static bool load_mock_file(
    const char *path,
    DtCupsPrinter *out_printers, int max_printers, int *out_pcount,
    const char *dest_filter,
    DtCupsJob *out_jobs, int max_jobs, int *out_jcount
) {
    if (out_pcount) *out_pcount = 0;
    if (out_jcount) *out_jcount = 0;
    FILE *fp = fopen(path, "r");
    if (!fp) return false;

    char line[1024];
    while (fgets(line, sizeof(line), fp)) {
        char *nl = strpbrk(line, "\r\n");
        if (nl) *nl = '\0';
        char *p = line;
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p || *p == '#') continue;

        if (strncmp(p, "PRINTER|", 8) == 0 && out_printers && out_pcount && *out_pcount < max_printers) {
            char *save = NULL;
            (void)strtok_r(p, "|", &save);
            const char *name = strtok_r(NULL, "|", &save);
            const char *uri = strtok_r(NULL, "|", &save);
            const char *model = strtok_r(NULL, "|", &save);
            const char *loc = strtok_r(NULL, "|", &save);
            const char *state = strtok_r(NULL, "|", &save);
            const char *acc = strtok_r(NULL, "|", &save);
            const char *def = strtok_r(NULL, "|", &save);
            const char *ippe = strtok_r(NULL, "|", &save);
            if (name && name[0]) {
                DtCupsPrinter *pr = &out_printers[(*out_pcount)++];
                memset(pr, 0, sizeof(*pr));
                safe_str_copy(pr->name, sizeof(pr->name), name);
                safe_str_copy(pr->device_uri, sizeof(pr->device_uri), uri ? uri : "ipp://localhost/printers/default");
                safe_str_copy(pr->make_model, sizeof(pr->make_model), model ? model : "IPP Everywhere Printer");
                safe_str_copy(pr->location, sizeof(pr->location), loc ? loc : "");
                safe_str_copy(pr->state, sizeof(pr->state), state ? state : "idle");
                pr->accepting = acc ? (atoi(acc) != 0) : true;
                pr->is_default = def ? (atoi(def) != 0) : false;
                pr->ipp_everywhere = ippe ? (atoi(ippe) != 0)
                                          : detect_ipp_everywhere(pr->device_uri, pr->make_model, NULL);
            }
        } else if (strncmp(p, "JOB|", 4) == 0 && out_jobs && out_jcount && *out_jcount < max_jobs) {
            char *save = NULL;
            (void)strtok_r(p, "|", &save);
            const char *id_s = strtok_r(NULL, "|", &save);
            const char *dest = strtok_r(NULL, "|", &save);
            const char *title = strtok_r(NULL, "|", &save);
            const char *user = strtok_r(NULL, "|", &save);
            const char *state = strtok_r(NULL, "|", &save);
            const char *sz_s = strtok_r(NULL, "|", &save);
            const char *d_s = strtok_r(NULL, "|", &save);
            const char *t_s = strtok_r(NULL, "|", &save);
            if (id_s && dest && (!dest_filter || !dest_filter[0] || strcmp(dest_filter, dest) == 0)) {
                DtCupsJob *jb = &out_jobs[(*out_jcount)++];
                memset(jb, 0, sizeof(*jb));
                jb->id = atoi(id_s);
                safe_str_copy(jb->dest, sizeof(jb->dest), dest);
                safe_str_copy(jb->title, sizeof(jb->title), title ? title : "Document");
                safe_str_copy(jb->user, sizeof(jb->user), user ? user : "user");
                safe_str_copy(jb->state, sizeof(jb->state), state ? state : "pending");
                jb->size_kb = sz_s ? atoi(sz_s) : 12;
                safe_str_copy(jb->date_str, sizeof(jb->date_str), d_s ? d_s : "2026-11-19");
                safe_str_copy(jb->time_str, sizeof(jb->time_str), t_s ? t_s : "12:00:00");
            }
        }
    }
    fclose(fp);
    return true;
}

extern "C" int DtCupsDiscoverPrinters(DtCupsPrinter *out_printers, int max_printers, bool *used_cups) {
    if (used_cups) *used_cups = false;
    if (!out_printers || max_printers <= 0) return 0;

    const char *mock = getenv("CK_CUPS_MOCK_FILE");
    if (mock && mock[0]) {
        int pcount = 0;
        if (load_mock_file(mock, out_printers, max_printers, &pcount, NULL, NULL, 0, NULL)) {
            if (used_cups) *used_cups = true;
            return pcount;
        }
    }

    void *lib = dlopen("libcups.so.2", RTLD_LAZY | RTLD_LOCAL);
    if (!lib) lib = dlopen("libcups.so", RTLD_LAZY | RTLD_LOCAL);
    if (!lib) return 0;

    int (*p_get_dests2)(void *, LocalCupsDest **) =
        (int (*)(void *, LocalCupsDest **))dlsym(lib, "cupsGetDests2");
    void (*p_free_dests)(int, LocalCupsDest *) =
        (void (*)(int, LocalCupsDest *))dlsym(lib, "cupsFreeDests");
    const char *(*p_get_opt)(const char *, int, LocalCupsOption *) =
        (const char *(*)(const char *, int, LocalCupsOption *))dlsym(lib, "cupsGetOption");

    int out_count = 0;
    if (p_get_dests2 && p_free_dests && p_get_opt) {
        if (used_cups) *used_cups = true;
        LocalCupsDest *dests = NULL;
        int ndests = p_get_dests2(NULL, &dests);
        for (int i = 0; i < ndests && out_count < max_printers; ++i) {
            if (!dests[i].name || !dests[i].name[0]) continue;
            DtCupsPrinter *pr = &out_printers[out_count++];
            memset(pr, 0, sizeof(*pr));
            if (dests[i].instance && dests[i].instance[0]) {
                snprintf(pr->name, sizeof(pr->name), "%s/%s", dests[i].name, dests[i].instance);
            } else {
                safe_str_copy(pr->name, sizeof(pr->name), dests[i].name);
            }
            const char *uri = p_get_opt("device-uri", dests[i].num_options, dests[i].options);
            const char *model = p_get_opt("printer-make-and-model", dests[i].num_options, dests[i].options);
            const char *loc = p_get_opt("printer-location", dests[i].num_options, dests[i].options);
            const char *pstate = p_get_opt("printer-state", dests[i].num_options, dests[i].options);
            const char *acc = p_get_opt("printer-is-accepting-jobs", dests[i].num_options, dests[i].options);
            const char *ptype = p_get_opt("printer-type", dests[i].num_options, dests[i].options);

            safe_str_copy(pr->device_uri, sizeof(pr->device_uri), uri ? uri : "ipp://localhost/printers");
            safe_str_copy(pr->make_model, sizeof(pr->make_model), model ? model : "CUPS Queue");
            safe_str_copy(pr->location, sizeof(pr->location), loc ? loc : "");
            safe_str_copy(pr->state, sizeof(pr->state), ipp_pstate_str(pstate));
            pr->accepting = (!acc || strcasecmp(acc, "false") != 0);
            pr->is_default = (dests[i].is_default != 0);
            pr->ipp_everywhere = detect_ipp_everywhere(uri, model, ptype);
        }
        if (dests && ndests > 0) {
            p_free_dests(ndests, dests);
        }
    }
    dlclose(lib);
    return out_count;
}

extern "C" int DtCupsDiscoverJobs(const char *dest_filter, DtCupsJob *out_jobs, int max_jobs) {
    if (!out_jobs || max_jobs <= 0) return 0;

    const char *mock = getenv("CK_CUPS_MOCK_FILE");
    if (mock && mock[0]) {
        int jcount = 0;
        if (load_mock_file(mock, NULL, 0, NULL, dest_filter, out_jobs, max_jobs, &jcount)) {
            return jcount;
        }
    }

    void *lib = dlopen("libcups.so.2", RTLD_LAZY | RTLD_LOCAL);
    if (!lib) lib = dlopen("libcups.so", RTLD_LAZY | RTLD_LOCAL);
    if (!lib) return 0;

    int (*p_get_jobs2)(void *, LocalCupsJob **, const char *, int, int) =
        (int (*)(void *, LocalCupsJob **, const char *, int, int))dlsym(lib, "cupsGetJobs2");
    void (*p_free_jobs)(int, LocalCupsJob *) =
        (void (*)(int, LocalCupsJob *))dlsym(lib, "cupsFreeJobs");

    int out_count = 0;
    if (p_get_jobs2 && p_free_jobs) {
        LocalCupsJob *jobs = NULL;
        /* myjobs=0 (all users), whichjobs=0 (CUPS_WHICHJOBS_ACTIVE) */
        int njobs = p_get_jobs2(NULL, &jobs, (dest_filter && dest_filter[0]) ? dest_filter : NULL, 0, 0);
        for (int i = 0; i < njobs && out_count < max_jobs; ++i) {
            DtCupsJob *jb = &out_jobs[out_count++];
            memset(jb, 0, sizeof(*jb));
            jb->id = jobs[i].id;
            safe_str_copy(jb->dest, sizeof(jb->dest), jobs[i].dest ? jobs[i].dest : "");
            safe_str_copy(jb->title, sizeof(jb->title), jobs[i].title ? jobs[i].title : "Untitled");
            safe_str_copy(jb->user, sizeof(jb->user), jobs[i].user ? jobs[i].user : "unknown");
            safe_str_copy(jb->state, sizeof(jb->state), ipp_jstate_str(jobs[i].state));
            jb->size_kb = jobs[i].size > 0 ? jobs[i].size : 1;
            format_epoch_date_time(jobs[i].creation_time, jb->date_str, sizeof(jb->date_str),
                                   jb->time_str, sizeof(jb->time_str));
        }
        if (jobs && njobs > 0) {
            p_free_jobs(njobs, jobs);
        }
    }
    dlclose(lib);
    return out_count;
}

extern "C" bool DtCupsFindPrinter(const char *name, DtCupsPrinter *out_printer) {
    if (!name || !name[0] || !out_printer) return false;
    DtCupsPrinter list[DT_CUPS_MAX_PRINTERS];
    bool used = false;
    int n = DtCupsDiscoverPrinters(list, DT_CUPS_MAX_PRINTERS, &used);
    for (int i = 0; i < n; ++i) {
        if (strcmp(list[i].name, name) == 0) {
            *out_printer = list[i];
            return true;
        }
    }
    return false;
}

extern "C" bool DtCupsCancelJob(const char *dest, int job_id) {
    if (job_id <= 0) return false;
    const char *mock = getenv("CK_CUPS_MOCK_FILE");
    if (mock && mock[0]) {
        /* Rewrite mock file omitting canceled job */
        FILE *fp = fopen(mock, "r");
        if (!fp) return false;
        char tmp_path[1024];
        snprintf(tmp_path, sizeof(tmp_path), "%s.cancel.tmp", mock);
        FILE *out = fopen(tmp_path, "w");
        if (!out) {
            fclose(fp);
            return false;
        }
        char line[1024];
        char prefix[64];
        snprintf(prefix, sizeof(prefix), "JOB|%d|", job_id);
        bool removed = false;
        while (fgets(line, sizeof(line), fp)) {
            if (strncmp(line, prefix, strlen(prefix)) == 0) {
                removed = true;
                continue;
            }
            fputs(line, out);
        }
        fclose(fp);
        fclose(out);
        if (removed && rename(tmp_path, mock) == 0) return true;
        remove(tmp_path);
        return false;
    }

    void *lib = dlopen("libcups.so.2", RTLD_LAZY | RTLD_LOCAL);
    if (!lib) lib = dlopen("libcups.so", RTLD_LAZY | RTLD_LOCAL);
    if (!lib) return false;

    int (*p_cancel)(const char *, int) =
        (int (*)(const char *, int))dlsym(lib, "cupsCancelJob");
    bool ok = false;
    if (p_cancel && dest && dest[0]) {
        ok = (p_cancel(dest, job_id) == 1);
    }
    dlclose(lib);
    return ok;
}

extern "C" int DtCupsPrintSummary(void) {
    DtCupsPrinter printers[DT_CUPS_MAX_PRINTERS];
    DtCupsJob jobs[DT_CUPS_MAX_JOBS];
    bool used_cups = false;
    int np = DtCupsDiscoverPrinters(printers, DT_CUPS_MAX_PRINTERS, &used_cups);
    int nj = DtCupsDiscoverJobs(NULL, jobs, DT_CUPS_MAX_JOBS);

    printf("CUPS_BACKEND=%s\n", used_cups ? "active" : "unavailable");
    printf("PRINTER_COUNT=%d\n", np);
    for (int i = 0; i < np; ++i) {
        const DtCupsPrinter *p = &printers[i];
        printf("PRINTER|%s|uri=%s|model=%s|location=%s|state=%s|accepting=%d|default=%d|ipp_everywhere=%d\n",
               p->name, p->device_uri, p->make_model, p->location,
               p->state, p->accepting ? 1 : 0, p->is_default ? 1 : 0, p->ipp_everywhere ? 1 : 0);
    }
    printf("JOB_COUNT=%d\n", nj);
    for (int j = 0; j < nj; ++j) {
        const DtCupsJob *jb = &jobs[j];
        printf("JOB|%d|dest=%s|title=%s|user=%s|state=%s|size_kb=%d|date=%s|time=%s\n",
               jb->id, jb->dest, jb->title, jb->user, jb->state, jb->size_kb,
               jb->date_str, jb->time_str);
    }
    return 0;
}
