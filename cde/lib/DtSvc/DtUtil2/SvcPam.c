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

/*
 * Header Files
 */

#include <stdlib.h>
#include <string.h>
#include <security/pam_appl.h>

/*
 * Local function declarations
 */

static int login_conv(int num_msg, const struct pam_message **msg,
        struct pam_response **response, void *appdata_ptr);

/*
 * Local structures and variables
 */

static struct pam_conv pam_conv = {login_conv, NULL};
static char *saved_user_passwd;
static pam_handle_t *pamh;

/*
 * End the handle, if there is one.  After pam_end() the handle is invalid
 * whatever it returned.
 */
static void PamEnd(int status)
{
    if (pamh) {
	(void) pam_end(pamh, status);
	pamh = NULL;
    }
}

/*
 * Get a handle that is set up for the service, user and display.  A handle
 * that is still there (from the authentication that this call follows) is
 * given the service and user again; one that cannot be set up is ended.
 */
static int PamStart(const char *service_name, const char *user,
		    const char *display_name)
{
    int status = PAM_SUCCESS;
    char *colon, *hostname;

    if (pamh) {
	if (service_name)
	    status = pam_set_item(pamh, PAM_SERVICE, service_name);

	if (status == PAM_SUCCESS && user)
	    status = pam_set_item(pamh, PAM_USER, user);
    }
    else {
	status = pam_start(service_name, user, &pam_conv, &pamh);

	if (status != PAM_SUCCESS) {
	    /* there is no handle to end */
	    pamh = NULL;
	    return status;
	}
    }

    if (status != PAM_SUCCESS) goto done;

    if (!display_name) goto done;

    if (display_name[0] == ':') {
	status = pam_set_item(pamh, PAM_TTY, display_name);
	goto done;
    }

    if (!(hostname = strdup(display_name))) {
	status = PAM_BUF_ERR;
	goto done;
    }

    if ((colon = strrchr(hostname, ':')) != NULL) *colon = '\0';

    status = pam_set_item(pamh, PAM_RHOST, hostname);

    free(hostname);

done:
    if (status != PAM_SUCCESS) PamEnd(status);

    return status;
}

/**
 * @brief Authenticate that user / password combination is legal for this
 *        system.
 *
 * Every call starts from a new handle: the user (and whatever else a module
 * remembered) of an earlier attempt must not decide whose password is
 * checked.  A password that is right for an account that may not be used (it
 * has expired, is locked, or the stack refuses it) is not accepted; only a
 * password that has to be changed is, which the caller sees as
 * PAM_NEW_AUTHTOK_REQD.  The handle stays for the session calls below when
 * the user may log in, and is ended when not.
 *
 * @param service_name
 * @param user
 * @param display_name
 * @param user_passwd
 *
 * @return See pam_authenticate and pam_acct_mgmt.
 */
int _DtSvcPamAuthenticate(const char *service_name, const char *user,
			  const char *display_name, const char *user_passwd)
{
    int status;

    if (!user_passwd) return PAM_AUTH_ERR;

    PamEnd(PAM_SUCCESS);

    if ((status = PamStart(service_name, user, display_name)) != PAM_SUCCESS)
	return status;

    /* the password is on loan to the conversation for this call only */
    saved_user_passwd = (char *) user_passwd;

    status = pam_authenticate(pamh, PAM_DISALLOW_NULL_AUTHTOK);

    if (status == PAM_SUCCESS)
	status = pam_acct_mgmt(pamh, PAM_DISALLOW_NULL_AUTHTOK);

    saved_user_passwd = NULL;

    if (status != PAM_SUCCESS && status != PAM_NEW_AUTHTOK_REQD)
	PamEnd(status);

    return status;
}

/**
 * @brief Start PAM session management.
 *
 * @param service_name
 * @param user
 * @param display_name
 *
 * @return See pam_open_session.
 */
int _DtSvcPamOpenSession(const char *service_name, const char *user,
			 const char *display_name)
{
    int status;

    if ((status = PamStart(service_name, user, display_name)) != PAM_SUCCESS)
	return status;

    return pam_open_session(pamh, 0);
}

/**
 * @brief Terminate PAM session management.
 *
 * @param service_name
 * @param user
 * @param display_name
 *
 * @return See pam_close_session.
 */
int _DtSvcPamCloseSession(const char *service_name, const char *user,
			  const char *display_name)
{
    int status;

    if ((status = PamStart(service_name, user, display_name)) != PAM_SUCCESS)
	return status;

    return pam_close_session(pamh, 0);
}

/**
 * @brief Set Users login credentials.
 *
 * @param service_name
 * @param user
 * @param display_name
 *
 * @return See pam_setcred.
 */
int _DtSvcPamSetcred(const char *service_name, const char *user,
		     const char *display_name)
{
    int status;

    if ((status = PamStart(service_name, user, display_name)) != PAM_SUCCESS)
	return status;

    return pam_setcred(pamh, PAM_ESTABLISH_CRED);
}

/*****************************************************************************
 * login_conv():
 *
 * This is a conv (conversation) function called from the PAM
 * authentication scheme.  It returns the user's password when requested by
 * internal PAM authentication modules.  The password is the one that
 * _DtSvcPamAuthenticate() has on loan; it answers every request for a hidden
 * answer with it and nothing else.  A request for a visible answer (a user
 * name), or for the password when there is none, fails the conversation.
 * The messages are an array of pointers to messages.
 *****************************************************************************/

static int login_conv(int num_msg, const struct pam_message **msg,
        struct pam_response **response, void *appdata_ptr)
{
    struct pam_response	*answers;
    char		*p;
    int			k;

    (void) appdata_ptr;

    if (num_msg <= 0 || num_msg > PAM_MAX_NUM_MSG || msg == NULL ||
	response == NULL)
        return (PAM_CONV_ERR);

    answers = (struct pam_response *) calloc(num_msg, sizeof (struct pam_response));
    if (answers == NULL)
        return (PAM_BUF_ERR);

    for (k = 0; k < num_msg; k++) {

        switch (msg[k]->msg_style) {

            case PAM_PROMPT_ECHO_OFF:
                if (saved_user_passwd == NULL ||
		    (answers[k].resp = strdup(saved_user_passwd)) == NULL)
		    goto failed;
                break;

            case PAM_ERROR_MSG:
            case PAM_TEXT_INFO:
                break;

            default:
		goto failed;
        }
    }

    *response = answers;
    return (PAM_SUCCESS);

failed:
    for (k = 0; k < num_msg; k++) {
	if (answers[k].resp != NULL) {
	    for (p = answers[k].resp; *p != '\0'; p++)
		*(volatile char *) p = '\0';
	    free(answers[k].resp);
	}
    }
    free(answers);
    return (PAM_CONV_ERR);
}
