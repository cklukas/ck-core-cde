/*
 * CDE - Common Desktop Environment
 *
 * Non-blocking desktop notification helper (org.freedesktop.Notifications
 * via notify-send) for Calendar Manager (dtcm) appointment reminders.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "notify_desktop.h"

int
dtcm_notify_reminder(const char *what, const char *time_range)
{
    const char *enabled = getenv("DTCM_DESKTOP_NOTIFY");
    if (enabled && (strcmp(enabled, "0") == 0 || strcmp(enabled, "false") == 0))
        return 0;

    const char *bin = getenv("DTCM_NOTIFY_BIN");
    if (!bin || !bin[0])
        bin = "notify-send";

    char title[256];
    if (time_range && time_range[0])
        snprintf(title, sizeof(title), "Calendar Reminder (%s)", time_range);
    else
        snprintf(title, sizeof(title), "Calendar Reminder");

    char body[512];
    if (what && what[0]) {
        snprintf(body, sizeof(body), "%s", what);
        /* Trim trailing newlines */
        size_t len = strlen(body);
        while (len > 0 && (body[len - 1] == '\n' || body[len - 1] == '\r'))
            body[--len] = '\0';
    } else {
        snprintf(body, sizeof(body), "Appointment reminder");
    }

    /* Double-fork so dtcm never blocks or leaves zombie children */
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        pid_t grand = fork();
        if (grand == 0) {
            char *argv[] = {
                (char *)bin,
                "--app-name=Calendar",
                "--icon=office-calendar",
                "--urgency=normal",
                "--",
                title,
                body,
                NULL
            };
            execvp(bin, argv);
            _exit(127);
        }
        _exit(0);
    }

    int status = 0;
    (void)waitpid(pid, &status, 0);
    return 0;
}
