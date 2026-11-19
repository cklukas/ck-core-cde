/*
 * CDE - Common Desktop Environment
 *
 * CupsDiscovery.h: Driverless CUPS / IPP Everywhere printer & job discovery
 * via libcups.so.2 (cupsGetDests2 / cupsGetJobs2 / cupsCancelJob2) with
 * deterministic $CK_CUPS_MOCK_FILE support for headless qualification.
 */

#ifndef DT_CUPS_DISCOVERY_H
#define DT_CUPS_DISCOVERY_H

#include <stdbool.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DT_CUPS_MAX_PRINTERS 64
#define DT_CUPS_MAX_JOBS     128

typedef struct {
    char name[128];
    char device_uri[256];
    char make_model[128];
    char location[128];
    char state[32];       /* "idle", "processing", "stopped" */
    bool accepting;
    bool is_default;
    bool ipp_everywhere;
} DtCupsPrinter;

typedef struct {
    int  id;
    char dest[128];
    char title[128];
    char user[64];
    char state[32];       /* "pending", "held", "processing", "completed", "canceled" */
    int  size_kb;
    char date_str[32];
    char time_str[32];
} DtCupsJob;

int  DtCupsDiscoverPrinters(DtCupsPrinter *out_printers, int max_printers, bool *used_cups);
int  DtCupsDiscoverJobs(const char *dest_filter, DtCupsJob *out_jobs, int max_jobs);
bool DtCupsFindPrinter(const char *name, DtCupsPrinter *out_printer);
bool DtCupsCancelJob(const char *dest, int job_id);
int  DtCupsPrintSummary(void);

#ifdef __cplusplus
}
#endif

#endif /* DT_CUPS_DISCOVERY_H */
