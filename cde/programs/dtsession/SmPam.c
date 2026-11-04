/*
 * CDE - Common Desktop Environment
 *
 * Copyright (c) 1993-2012, The Open Group. All rights reserved.
 *
 * These libraries and programs are free software; you can
 * redistribute them and/or modify them under the terms of the GNU
 * Lesser General Public License as published by the Free Software
 * Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * These libraries and programs are distributed in the hope that
 * they will be useful, but WITHOUT ANY WARRANTY; without even the
 * implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 * PURPOSE. See the GNU Lesser General Public License for more
 * details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with these libraries and programs; if not, write
 * to the Free Software Foundation, Inc., 51 Franklin Street, Fifth
 * Floor, Boston, MA 02110-1301 USA
 */
/*************************************<+>*************************************
 **
 **  File:        SmPam.c
 **
 **  Project:     DT Session Manager (dtsession)
 **
 **  Description:
 **  -----------
 **  The password check of the screen lock.  See SmPam.h.
 **
 *************************************<+>*************************************/

#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <pwd.h>
#include <unistd.h>

#ifdef HAS_PAM_LIBRARY
#include <security/pam_appl.h>
#endif

#include "SmPam.h"

/*
 * Copy of the name of the account of the user who started the program.  The
 * account is found by the real user ID, nothing else.
 */
char *
SmPamUserName(void)
{
    struct passwd *entry;
    char *name = NULL;

    entry = getpwuid(getuid());
    if (entry != NULL && entry->pw_name != NULL && entry->pw_name[0] != '\0')
    {
        name = strdup(entry->pw_name);
    }
    endpwent();

    return name;
}

#ifdef HAS_PAM_LIBRARY

/*
 * The PAM service of the lock is fixed.  It is not derived from the name the
 * program was started with, which the invoker chooses.
 */
#define SM_PAM_SERVICE "dtsession"

/*
 * Overwrite a password in memory.  The stores go through a volatile pointer
 * so that the compiler does not drop them as dead.
 */
static void
WipeString(
        char *string )
{
    volatile char *p = string;

    while (*p != '\0')
    {
        *p++ = '\0';
    }
}

/*
 * The conversation with the modules of the PAM stack.  The only question the
 * lock can answer is the one for the password, with the password that was
 * typed; the modules' messages are not shown.  A request that wants anything
 * else (a visible answer such as a user name, which would let the stack
 * authenticate someone other than the session's user) fails the
 * conversation.
 */
static int
Converse(
        int count,
        const struct pam_message **messages,
        struct pam_response **responses,
        void *data )
{
    const char *password = data;
    struct pam_response *answers;
    int i;

    if (count <= 0 || count > PAM_MAX_NUM_MSG || messages == NULL ||
        responses == NULL || password == NULL)
    {
        return PAM_CONV_ERR;
    }

    answers = calloc(count, sizeof(*answers));
    if (answers == NULL)
    {
        return PAM_CONV_ERR;
    }

    for (i = 0; i < count; i++)
    {
        switch (messages[i]->msg_style)
        {
            case PAM_PROMPT_ECHO_OFF:
                answers[i].resp = strdup(password);
                if (answers[i].resp == NULL)
                {
                    goto failed;
                }
                break;

            case PAM_ERROR_MSG:
            case PAM_TEXT_INFO:
                break;

            default:
                goto failed;
        }
    }

    *responses = answers;
    return PAM_SUCCESS;

failed:
    for (i = 0; i < count; i++)
    {
        if (answers[i].resp != NULL)
        {
            WipeString(answers[i].resp);
            free(answers[i].resp);
        }
    }
    free(answers);
    return PAM_CONV_ERR;
}

Boolean
SmPamAvailable(void)
{
    char *name = SmPamUserName();

    if (name == NULL)
    {
        return False;
    }
    free(name);

    return True;
}

Boolean
SmPamCheckPassword(
        const char *password )
{
    struct pam_conv conversation;
    pam_handle_t *handle = NULL;
    Boolean accepted = False;
    char *name;
    int status;

    if (password == NULL)
    {
        return False;
    }

    name = SmPamUserName();
    if (name == NULL)
    {
        return False;
    }

    conversation.conv = Converse;
    conversation.appdata_ptr = (void *) password;

    status = pam_start(SM_PAM_SERVICE, name, &conversation, &handle);
    free(name);
    if (status != PAM_SUCCESS)
    {
        return False;
    }

    /*
     * An empty password is never accepted.  The account has to be in order
     * as well, not only the password right: an expired, locked or denied
     * account does not unlock the display.  The one exception is a password
     * that expired while the user was logged in (PAM_NEW_AUTHTOK_REQD from
     * the account check): the user is inside the session already, and the
     * change of the password belongs to the next login.  An authentication
     * that fails, whatever it fails with, is refused.
     */
    status = pam_authenticate(handle, PAM_DISALLOW_NULL_AUTHTOK);
    if (status == PAM_SUCCESS)
    {
        status = pam_acct_mgmt(handle, PAM_DISALLOW_NULL_AUTHTOK);
        accepted = (status == PAM_SUCCESS || status == PAM_NEW_AUTHTOK_REQD)
                   ? True : False;
    }
    (void) pam_end(handle, status);

    return accepted;
}

#else /* !HAS_PAM_LIBRARY */

Boolean
SmPamAvailable(void)
{
    return False;
}

Boolean
SmPamCheckPassword(
        const char *password )
{
    (void) password;
    return False;
}

#endif /* HAS_PAM_LIBRARY */
