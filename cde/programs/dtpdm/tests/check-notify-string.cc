/* Harness for check-notify-string.py: PdmOidNotifyString() must return a
 * value for every argument, including one outside the enumeration. */
/* @@PROLOGUE@@ */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <assert.h>
#define NOTIFY_EMAIL_STR "{{event-report-job-completed} electronic-mail}"
#define NOTIFY_NONE_STR  "{}"
typedef enum {
    PDMOID_NOTIFY_UNSUPPORTED,
    PDMOID_NOTIFY_NONE,
    PDMOID_NOTIFY_EMAIL
} PdmOidNotify;
/* @@BODY@@ */
int main(void)
{
    assert(PdmOidNotifyString(PDMOID_NOTIFY_UNSUPPORTED) == NULL);
    assert(strcmp(PdmOidNotifyString(PDMOID_NOTIFY_NONE), "{}") == 0);
    assert(strcmp(PdmOidNotifyString(PDMOID_NOTIFY_EMAIL), NOTIFY_EMAIL_STR) == 0);
    assert(PdmOidNotifyString((PdmOidNotify) 99) == NULL);
    puts("notify string ok");
    return 0;
}
