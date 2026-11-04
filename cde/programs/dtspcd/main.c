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
 * File:         main.c $TOG: main.c /main/8 1999/09/30 15:31:41 mgreess $
 * Language:     C
 *
 * (c) Copyright 1988, Hewlett-Packard Company, all rights reserved.
 *
 * (c) Copyright 1993, 1994 Hewlett-Packard Company			*
 * (c) Copyright 1993, 1994 International Business Machines Corp.	*
 * (c) Copyright 1993, 1994 Sun Microsystems, Inc.			*
 * (c) Copyright 1993, 1994 Novell, Inc.				*
 */

#include <bms/sbport.h>

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>			/* ctime() */
#include <pwd.h>
#include <errno.h>
#include <signal.h>

#include <unistd.h>			/* initgroups() */
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <grp.h>

#include <string.h>
#include <libgen.h>			/* basename() */
#include <sys/param.h>			/* MAXPATHLEN */

#include <bms/bms.h>
#include <bms/MemoryMgr.h>
#include <bms/XeUserMsg.h>
#include <bms/usersig.h>
#include <bms/spc.h>

#include <SPC/spcP.h>		
#include <SPC/spc-proto.h>
#include <XlationSvc.h>
#include <LocaleXlate.h>

#include <Tt/tt_c.h>

#include "spcd_event.h"

static char *MOUNT_POINT = "DTMOUNTPOINT";

/* Forward declarations */
/** ------------------ **/
static void Merge_Lang_Var(SPC_Channel_Ptr channel);

void SPCD_Handle_Client_Data(void *channel, int *source, SbInputId *id );
int SPCD_Initialize(void);
[[noreturn]] void SPCD_Exit(int exitval);
void SPCD_Handle_Application_Data(void *chn, XeString text, int size, int conn_type );
void SPCD_Termination_Handler(SPC_Channel_Ptr channel, int pid, int type, int cause, void *data );
int Client_Abort(protocol_request_ptr prot);
int Client_Register(protocol_request_ptr prot);
int Client_Unregister(protocol_request_ptr prot);
int Client_Channel_Open(protocol_request_ptr prot);
int Client_Channel_Close(protocol_request_ptr prot);
int Client_Channel_Reset(protocol_request_ptr prot);
int Client_Channel_Attach(protocol_request_ptr prot);
int Client_Application_Spawn(protocol_request_ptr prot);
int Client_Application_Signal(protocol_request_ptr prot);
int Client_Application_Data(protocol_request_ptr prot);
int Client_Server_Debug(protocol_request_ptr prot);
int Client_Environ_Reset(protocol_request_ptr prot);
int Client_Reply_Devices(protocol_request_ptr prot);
int Client_Reply_Logfile(protocol_request_ptr prot);
int Client_Delete_Logfile(protocol_request_ptr prot);
int Client_Reset_Termios(protocol_request_ptr prot);
int Client_Protocol_Version(protocol_request_ptr prot);
int Client_Reply_Protocol(protocol_request_ptr prot);
int Client_Reply_Hostinfo(protocol_request_ptr prot);

/* New B.00 methods */

int Client_Send_EOF(protocol_request_ptr prot);
int Client_Channel_Termios(protocol_request_ptr prot);
int Client_Enhanced_Spawn(protocol_request_ptr prot);

#if defined(_AIX) || defined(__linux__)
# define SA_HANDLER_INT_ARG
#endif /* _AIX */

int conditional_putenv(XeString env_str);
void SPCD_Reply(SPC_Connection_Ptr connection, protocol_request_ptr prot, int retval, int errval );
#ifdef SA_HANDLER_INT_ARG
void SPCD_Alarm_Handler (int);
#else
void SPCD_Alarm_Handler (void);
#endif /* SA_HANDLER_INT_ARG */

/* Global data */

protocol_request_handler spcd_protocol_dispatch_table[NREQS]={
  Client_Application_Data,   /* APPLICATION_DATA */
  NULL,                      /* APPLICATION_STDOUT */
  NULL,                      /* APPLICATION_STDERR */
  Client_Abort,              /* ABORT */
  Client_Register,           /* REGISTER */
  Client_Unregister,         /* UNREGISTER */
  Client_Channel_Open,       /* CHANNEL_OPEN */
  Client_Channel_Close,      /* CHANNEL_CLOSE */
  Client_Channel_Reset,      /* CHANNEL_RESET */
  Client_Channel_Attach,     /* CHANNEL_ATTACH */
  Client_Application_Spawn,  /* APPLICATION_SPAWN */
  Client_Application_Signal, /* APPLICATION_SIGNAL */
  NULL,                      /* APPLICATION_DIED */
  NULL,                      /* SERVER_ERROR */
  NULL,                      /* REPLY */
  Client_Server_Debug,       /* SERVER_DEBUG */
  Client_Environ_Reset,      /* ENVIRON_RESET */
  Client_Reply_Devices,      /* QUERY_DEVICES */
  NULL,                      /* DEVICE_REPLY */
  Client_Reply_Logfile,      /* QUERY_LOGFILE */
  NULL,                      /* LOGFILE_REPLY */
  Client_Delete_Logfile,     /* DELETE_LOGFILE */
  Client_Reset_Termios,      /* RESET_TERMIOS */
  Client_Send_EOF,           /* CHANNEL_SEND_EOF */
  Client_Channel_Termios,    /* CHANNEL_TERMIOS */
  Client_Enhanced_Spawn,     /* APP_B00_SPAWN */
};

/*
 * HomeDir will be set to "HOME=pwent->pw_dir".
 *
 * ShellDir will be set to "SHELL=pwent->pw_shell".
 */
XeChar HomeDir[MAXPATHLEN + 6];
XeChar ShellDir[MAXPATHLEN + 7];
XeString *default_environment;

int client_validated=0;
int SPCD_Abort_Okay = FALSE;

SPC_Connection_Ptr client_connection;

SPC_Channel client_channel;

/*
 * This variable contains the number of minutes in the daemon's
 * exit timer.  If the daemon has no activity within this period 
 * of time and there are no sub-processes running, the daemon
 * will exit.
 */
static int exit_timeout = SPCD_DEFAULT_TIMEOUT;

/*
 * The following variable is used by the timer code to indicate
 * if a request is currently being serviced.  This is necessary
 * because of the following scenario:
 *
 *   t0 - a request is made
 *   t1 - the request is serviced
 *   t2 - the timer expires
 *   t3 - the timer handler sees no sub-processes running so it exits
 *   t4 - request is done, reply to client
 *
 * This variable is set to SPCD_REQUEST_PENDING at t0 and to
 * SPCD_NO_REQUST_PENDING at t4.  If at t3 the variable is set
 * to SPCD_REQUEST_PENDING, the alarm will be reset and the
 * daemon will continue.
 */
static int request_pending = SPCD_NO_REQUEST_PENDING;

/*----------------------------------------------------------------------+*/
int main(int argc, XeString *argv)
/*----------------------------------------------------------------------+*/
{
  /* Parse the command line and set globals accordingly. */
  XeString log_path = NULL;
  Boolean terminate_flag = FALSE;
  int i;
  struct sigaction alarm_vector;
  char tmp[200];
  char *pch;
  
  /*
   * The SPC library needs to know this is a SPC 'daemon' process
   * to ensure 'SPC_Initialize()' installs a SIGCLD handler.
   */
  SPC_who_am_i = SPC_I_AM_A_DAEMON;
  SPCD_Authentication_Dir = NULL;

  /* set up log file path */
  log_path = XeSBTempPath((XeString)"DTSPCD.log");

  if(NULL == freopen("/dev/null", "w", stderr)) {
    printf("Unable to open /dev/null\n");
    exit(EXIT_FAILURE);
  }
  
  /* Process arguments and set flags.  */
  for (i=1; i < argc; i++) {

    if (!strcmp ("-log", argv[i])) {
      /* Log mode.  Print information to a log file */
      /* Open an error log with whatever name the library wants to use */
      SPC_Open_Log(log_path, FALSE);
    }

    else if (!strcmp ("-debug", argv[i])) {
      /* Debug mode.  Print protocol information to a log file */
      /* Open an error log with whatever name the library wants to use */
      SPC_Open_Log(log_path, FALSE);
      SPC_Print_Protocol = spc_logF;
      if(NULL == freopen(log_path, "a", stderr)) {
        printf("Unable to reopen '%s' as stderr\n", log_path);
        exit(EXIT_FAILURE);
      }
      setbuf(stderr, NULL);
    }

    else if (!strcmp ("-auth_dir", argv[i])) {
      /* 
       * Used to override the default directory for authentication file 
       */
      i++;
      if (i != argc) {
	SPC_Format_Log((XeString)"Authentication directory set to '%s'.", 
		       argv[i]);
        SPCD_Authentication_Dir = strdup (argv[i]);
      }
    }

    else if (!strcmp ("-mount_point", argv[i])) {
      /* 
       * Mount point for the filename mapping system.
       */
      i++;
      if (i != argc) {
	(void) snprintf (tmp, sizeof(tmp), "%s=%s", MOUNT_POINT, argv[i]);
	if (putenv (tmp) == 0) {
	  SPC_Format_Log((XeString)"Mount point set to '%s'.", argv[i]);
	  SPC_mount_point_env_var = (char *) malloc (strlen (tmp) + 1);
	  (void) strcpy (SPC_mount_point_env_var, tmp);
	}
	else
	  SPC_Format_Log((XeString)"Failed to add the mount point '%s' to the environment.", tmp);
      }
    }

    else if (!strcmp ("-timeout", argv[i])) {
      /* 
       * The timeout is specified, so use it instead of the default.
       */
      i++;
      if (i != argc) {
	exit_timeout = atoi(argv[i]);
	SPC_Format_Log((XeString)"Setting the exit timer to '%s' minutes.", 
		       argv[i]);
      }
    }

    else {
      /* 
       * Unknown command option 
       */
      SPC_Format_Log((XeString)"Command line option '%s' unrecognized.", 
		     argv[i]);
    }
  }

  /* free strings allocated for path */
  XeFree(log_path);

  /*
   * Initialize the i/o function pointers.
   */
  SbAddInput_hookfn        = SPCD_AddInput;
  SbAddException_hookfn    = SPCD_AddException;
  SbRemoveInput_hookfn     = SPCD_RemoveInput;
  SbRemoveException_hookfn = SPCD_RemoveException;
  SbMainLoopUntil_hookfn   = SPCD_MainLoopUntil;
  SbBreakMainLoop_hookfn   = SPCD_BreakMainLoop;

  /* Initialization Commands */
  if (SPCD_Initialize()==SPC_ERROR)
    SPCD_Exit(1);

  client_connection=SPC_Start_Daemon(FALSE);
  if(client_connection==SPC_ERROR)
    SPCD_Exit(1);

  client_channel.connection = client_connection;
  
  SPC_XtAddInput(&client_channel,
		 &client_connection->termination_id,
		 client_connection->sid,
		 SPCD_Handle_Client_Data,
		 SPC_Client);
  
  if (exit_timeout != SPCD_NO_TIMER) {
    memset(&alarm_vector, 0, sizeof(struct sigaction));
    alarm_vector.sa_handler = SPCD_Alarm_Handler;
    alarm_vector.sa_flags = 0;
    (void) sigaction (SIGALRM, &alarm_vector, (struct sigaction *)NULL);
    (void) alarm (exit_timeout * 60);
  }

  /*
   * The daemon's mount point environment variable needs to be 
   * saved.  It will be used to override the client's mount point
   * setting or the mount point specified in any of the environment 
   * files.
   */
  if (SPC_mount_point_env_var == NULL)
    if ((pch = getenv (MOUNT_POINT)) != NULL) {
      SPC_mount_point_env_var = (char *) malloc (strlen (pch) + 
						 strlen (MOUNT_POINT) + 2);
      (void) sprintf (SPC_mount_point_env_var, "%s=%s", MOUNT_POINT, pch);
    }

  XeCall_SbMainLoopUntil(&terminate_flag);
  SPCD_Exit(0);
}

/* The requests that act on a channel of their own; the others (register,
   open, environment and debug requests) do not name one. */
/*----------------------------------------------------------------------+*/
static int Request_Needs_Channel(int type)
/*----------------------------------------------------------------------+*/
{
  switch(type) {
  case APPLICATION_DATA:
  case CHANNEL_CLOSE:
  case CHANNEL_RESET:
  case CHANNEL_ATTACH:
  case APPLICATION_SPAWN:
  case APPLICATION_SIGNAL:
  case QUERY_DEVICES:
  case QUERY_LOGFILE:
  case DELETE_LOGFILE:
  case CHANNEL_SEND_EOF:
  case CHANNEL_TERMIOS:
  case APP_B00_SPAWN:
    return(TRUE);
  default:
    return(FALSE);
  }
}

/*----------------------------------------------------------------------+*/
void SPCD_Handle_Client_Data(void      *channel,
			     int       *UNUSED_PARM(source),
			     SbInputId *UNUSED_PARM(id))
/*----------------------------------------------------------------------+*/
{
  protocol_request_ptr prot;
  int retval;
  SPC_Connection_Ptr connection=((SPC_Channel_Ptr)channel)->connection;

  request_pending = SPCD_REQUEST_PENDING;

  prot=SPC_Read_Protocol(connection);
  if(!prot)
    SPCD_Exit(0);

  /* Check for valid client (in other words, that we have registered
     this client). */

  if((!client_validated) && (prot->request_type != REGISTER)) {
    SPC_Error(SPC_Client_Not_Valid);
    if (exit_timeout != SPCD_NO_TIMER)
      (void) alarm (exit_timeout * 60);
    request_pending = SPCD_REQUEST_PENDING;
    SPC_Free_Protocol_Ptr(prot);
    return;
  }

  SPCD_Abort_Okay = TRUE;
  if(Request_Needs_Channel(prot->request_type) && prot->channel == NULL) {
    /* The channel id names no channel of this client. */
    SPC_Error(SPC_Protocol);
    retval = SPC_ERROR;
  } else
    retval=SPC_Dispatch_Protocol(prot, spcd_protocol_dispatch_table);
  SPCD_Abort_Okay = FALSE;
  if(REPLY_EXPECTED(prot->request_type, TRUE) != NO_REPLY_VAL)
    SPCD_Reply(connection, prot, retval, errno);
  SPC_Free_Protocol_Ptr(prot);

  /* 
   * Reset the alarm and go back to select.
   */
  if (exit_timeout != SPCD_NO_TIMER)
    (void) alarm (exit_timeout * 60);

  request_pending = SPCD_NO_REQUEST_PENDING;
}

/*----------------------------------------------------------------------+*/
int SPCD_Initialize(void)
/*----------------------------------------------------------------------+*/
{
  XeString sys_env_file = NULL;
  
  /* Do initialization for SPC */
  if(SPC_Initialize()==SPC_ERROR)
    return(SPC_ERROR);

  /* Do Daemon specific initialization */

  /* Error handling */

  XeProgName=SPCD_PROG_NAME;

  default_environment=
    (XeString *) malloc(DEFAULT_ENVP_SIZE * sizeof(XeString));
  default_environment[0]=NULL;

  /*
   * First add the installed environment file.
   */
  sys_env_file = (XeString) malloc (strlen(SPCD_ENV_INSTALL_DIRECTORY) +
				    strlen(SPCD_ENV_FILE) + 3);
  (void) sprintf (sys_env_file, "%s/%s", 
		  SPCD_ENV_INSTALL_DIRECTORY, 
		  SPCD_ENV_FILE);
  default_environment=SPC_Add_Env_File(sys_env_file,default_environment);

  /*
   * Now add the configured environment file.
   */
  sys_env_file = (XeString) realloc (sys_env_file,
				     strlen(SPCD_ENV_CONFIG_DIRECTORY) +
				     strlen(SPCD_ENV_FILE) + 3);
  (void) sprintf (sys_env_file, "%s/%s", 
		  SPCD_ENV_CONFIG_DIRECTORY, 
		  SPCD_ENV_FILE);

  default_environment=SPC_Add_Env_File(sys_env_file,default_environment);

  free (sys_env_file);

  return(TRUE);
}

/*----------------------------------------------------------------------+*/
void SPCD_Exit(int exitval)
/*----------------------------------------------------------------------+*/
{
  int i;

  if (SPC_logfile_list != NULL)
    for (i = 0; SPC_logfile_list[i] != NULL; i++)
      (void) unlink (SPC_logfile_list[i]);

  SPC_Format_Log((XeString)"Exiting server.  Retval: %d", exitval);
  SPC_Close_Log();

  exit(exitval);
}

/*----------------------------------------------------------------------+*/
void SPCD_Handle_Application_Data(void * chn,
                                  XeString text,
                                  int size,
                                  int conn_type )
/*----------------------------------------------------------------------+*/
{
  SPC_Channel_Ptr channel= (SPC_Channel_Ptr) chn;
  SPC_Write_Protocol_Request(client_connection, channel,
			     CONNECTOR_TO_PROT(conn_type), text, size);
}


/*----------------------------------------------------------------------+*/
void SPCD_Termination_Handler(SPC_Channel_Ptr channel,
                              int UNUSED_PARM(pid),
                              int UNUSED_PARM(type),
                              int UNUSED_PARM(cause),
			      void * UNUSED_PARM(data))
/*----------------------------------------------------------------------+*/
{
  /* Write a termination protocol request */

  SPC_Write_Protocol_Request(client_connection, channel,
			     APPLICATION_DIED, channel->status);

  /* That's all, folks! */
}

/*
 ***
 *** Protocol request handlers
 ***
*/

/*----------------------------------------------------------------------+*/
int Client_Abort(protocol_request_ptr prot)
/*----------------------------------------------------------------------+*/
{
  return(print_protocol_request((XeString)"--> ABORT", prot));
}


/* Sixteen hexadecimal digits from the system's random source.  The name of
   the authentication file is chosen by the daemon, and no other process may be
   able to guess it. */
/*----------------------------------------------------------------------+*/
static int Authentication_Suffix(char *suffix, size_t size)
/*----------------------------------------------------------------------+*/
{
  unsigned char raw[8];
  size_t i;
  ssize_t got;
  int fd;

  if(size < 2 * sizeof(raw) + 1)
    return(FALSE);
  fd = open("/dev/urandom", O_RDONLY);
  if(fd < 0)
    return(FALSE);
  do
    got = read(fd, raw, sizeof(raw));
  while(got < 0 && errno == EINTR);
  close(fd);
  if(got != (ssize_t)sizeof(raw))
    return(FALSE);
  for(i = 0; i < sizeof(raw); i++)
    snprintf(suffix + 2 * i, size - 2 * i, "%02x", raw[i]);
  return(TRUE);
}

/* Give up the daemon's identity for the authenticated user's.  inetd starts
   the daemon as root, and every command the client then asks for would run with
   that authority if any step here did not take effect, so each step is
   checked and the caller stops the daemon on failure. */
/*----------------------------------------------------------------------+*/
static int Become_User(const char *name, uid_t uid, gid_t gid)
/*----------------------------------------------------------------------+*/
{
  if(geteuid() == 0 && initgroups(name, gid) == -1) {
    SPC_Format_Log((XeString)"+++> FAILURE: initgroups ('%s', %u): %s",
		   name, (unsigned int)gid, strerror(errno));
    return(FALSE);
  }
  if((getegid() != gid || getgid() != gid) && setgid(gid) == -1) {
    SPC_Format_Log((XeString)"+++> FAILURE: setgid (%u): %s",
		   (unsigned int)gid, strerror(errno));
    return(FALSE);
  }
  if((geteuid() != uid || getuid() != uid) && setuid(uid) == -1) {
    SPC_Format_Log((XeString)"+++> FAILURE: setuid (%u): %s",
		   (unsigned int)uid, strerror(errno));
    return(FALSE);
  }
  if(getegid() != gid || getgid() != gid ||
     geteuid() != uid || getuid() != uid) {
    SPC_Format_Log((XeString)"+++> FAILURE: the process identity did not change to '%s'.", name);
    return(FALSE);
  }
  /* A process that held root must not be able to take it back. */
  if(uid != 0 && setuid(0) == 0) {
    SPC_Format_Log((XeString)"+++> FAILURE: root privileges were not dropped.");
    return(FALSE);
  }
  return(TRUE);
}

/* Refuse the registration: tell the client, report the error and finish. */
#define REFUSE_REGISTRATION(code) \
  do { \
    SPC_Write_Protocol_Request(client_connection, NULL, LOGFILE_REPLY, \
			       FAILED_FILE_NAME, NULL, NULL); \
    SPC_Error(code); \
    goto done; \
  } while(0)

/*----------------------------------------------------------------------+*/
int Client_Register(protocol_request_ptr prot)
/*----------------------------------------------------------------------+*/
{
  XeString username = NULL;
  XeString passwd = NULL;
  XeString proto_ver = NULL;
  XeString hostinfo = NULL;
  struct passwd *pwent;
  XeString tmpfile = NULL, tmppath = NULL;
  int free_tmppath = 0;
  XeChar  buffer[MAXPATHLEN * 2];
  struct stat buf;
  XeString netfile = NULL;
  int free_netfile = 0;
  XeString env_file = NULL;
  protocol_request_ptr reply;
  const char *spc_prefix = "/.SPC_";
  char suffix[2 * 8 + 1];
  size_t buffsize;
  uid_t uid;
  gid_t gid;
  int version;
  int result = SPC_ERROR;

  print_protocol_request((XeString)"--> REGISTER", prot);
  prot->channel=0;

  if(READ_REGISTER(prot->dataptr, username, passwd, proto_ver, hostinfo) != TRUE) {
    SPC_Format_Log((XeString)"+++> FAILURE: malformed REGISTER request.");
    REFUSE_REGISTRATION(SPC_Protocol);
  }

  if(username[0] == '\0') {
    /* No username was supplied (this could happen with pre-CDE clients). */
    SPC_Format_Log((XeString)"+++> FAILURE: NULL username.");
    REFUSE_REGISTRATION(SPC_Bad_Username);
  }

  SPC_Format_Log ((XeString)"+++> Starting authentication for user '%s'\n     from host '%s'.",
		  username, hostinfo);
  /*
   * We have a username so generate a temp filename and send it
   * back to the client after creating the proper path for the file.
   */
  if(SPCD_Authentication_Dir)
    /*
     * Use the directory specified on the command line.
     */
    tmppath = SPCD_Authentication_Dir;
  else {
    /*
     * Use the $HOME directory if it can be retrieved from the
     * password file.
     */
    if(!(pwent=getpwnam(username))) {
      SPC_Format_Log ((XeString)"+++> FAILURE: username '%s' is unknown.", username);
      REFUSE_REGISTRATION(SPC_Bad_Username);
    }
    tmppath = (XeString)(pwent->pw_dir);

    if ((lstat (tmppath, &buf) != 0) ||
	(!S_ISDIR(buf.st_mode))     ||
	(!(buf.st_mode & S_IRUSR))) {
      /*
       * Use the default directory.
       */
      tmppath = XeSBTempPath(XeString_NULL);
      free_tmppath = 1;
    }
  }

  /*
   * The file name is built here from a random suffix.  tempnam(3) can ignore
   * the directory it is given, and tmpnam(3) names are predictable.
   */
  if(!Authentication_Suffix(suffix, sizeof(suffix))) {
    SPC_Format_Log((XeString)"+++> FAILURE: cannot generate an authentication file name.");
    REFUSE_REGISTRATION(SPC_Bad_Authentication);
  }

  /* Allocate space for tmppath, spc_prefix, and the suffix. */
  buffsize = strlen(tmppath) + strlen(spc_prefix) + strlen(suffix) + 1;
  tmpfile = (char *)malloc(buffsize);
  if(!tmpfile) {
    SPC_Format_Log((XeString)"+++> FAILURE: cannot malloc.");
    REFUSE_REGISTRATION(SPC_Out_Of_Memory);
  }
  snprintf(tmpfile, buffsize, "%s%s%s", tmppath, spc_prefix, suffix);
  if(free_tmppath) {
    XeFree(tmppath);
    free_tmppath = 0;
  }
  tmppath = NULL;

  /*
   * Store info about the client (protocol version & host type)
   * For Pre A.01, this will be defaulted to (hpux 7.0 s300)
   */
  client_connection->hostinfo = hostinfo;
  hostinfo = NULL;			/* the connection owns it now */

  if (proto_ver) {
      if (sscanf(proto_ver, "%d", &version) == 1)
	  client_connection->protocol_version = version;
      SPC_client_version_number = client_connection->protocol_version;
  }
  else
      /* The client didn't send a protocol_version so set it to "1". */
      SPC_client_version_number = 1;

  SPC_Format_Log((XeString)"     Client protocol version is '%d'.",
		 SPC_client_version_number);
  SPC_Format_Log((XeString)"+++> Authentication file is '%s'.", tmpfile);

  /*
   * For non-CDE clients, the clients expect the daemon to send
   * a "real" file name.  Beginning with 'SPC_PROTOCOL_VERSION_CDE_BASE'
   * the clients are expecting a "netfile" name.
   */
  netfile = tmpfile;

  if (SPC_client_version_number >= SPC_PROTOCOL_VERSION_CDE_BASE) {
     netfile = tt_file_netfile (tmpfile);

     if (tt_ptr_error (netfile) != TT_OK) {
       SPC_Format_Log((XeString)"+++> FAILURE: cannot create a cannonical file name for the authentication file.\n     (%s)",
		      tt_status_message(tt_pointer_error(netfile)));
       netfile = NULL;
       REFUSE_REGISTRATION(SPC_Bad_Authentication);
     }
     free_netfile = 1;
     SPC_Format_Log((XeString)"     Authentication 'netfile' is '%s'.", netfile);
  }

  /* As we send the next packet, include protocol and host info about */
  /* us (the server).  If going to a pre A.02 client, it will be      */
  /* ignored by it.                                                   */

  SPC_Write_Protocol_Request(client_connection, NULL, LOGFILE_REPLY,
			     netfile, SPC_PROTOCOL_VERSION_STR,
			     SPC_LocalHostinfo());

  reply=SPC_Filter_Connection(client_connection, NULL, REGISTER, TRUE);
  if(reply==SPC_ERROR)
    goto done;
  snprintf(buffer, sizeof(buffer), "--> REGISTER (%s)", netfile);
  print_protocol_request(buffer, reply);
  SPC_Free_Protocol_Ptr(reply);

  /* Was the client able to create the authentication */
  /* file in the temp directory?		        */

  if(lstat(tmpfile, &buf)==ERROR) {
    int terrno = errno;
    SPC_Format_Log((XeString)"+++> FAILURE: lstat authentication file '%s'.", tmpfile);
    SPC_Format_Log((XeString)"+++> FAILURE: lstat() returned error '%s'\n",
		   strerror(terrno));
    REFUSE_REGISTRATION(SPC_Bad_Authentication);
  }

  if (S_ISLNK(buf.st_mode))
    {				/* somebody is jerkin us around */
      SPC_Format_Log((XeString)"+++> FAILURE: lstat authentication file '%s' is a symlink! Possible compromise attempt.", tmpfile);
      REFUSE_REGISTRATION(SPC_Bad_Authentication);
    }

  /*
   * The file must be a plain file with one name.  A hard link to another
   * user's setuid file passes every test below with that user's identity.
   */
  if (!S_ISREG(buf.st_mode) || buf.st_nlink != 1)
    {
      SPC_Format_Log((XeString)"+++> FAILURE: authentication file '%s' is not a plain file with a single link. Possible compromise attempt.", tmpfile);
      REFUSE_REGISTRATION(SPC_Bad_Authentication);
    }

  /*
   * If the file does not have the setuid bit set then return failure.
   *
   * Note that if the protocol_version is < 2, this bit will
   * not be set and the client will not be able to connect.
   */
  if(!(buf.st_mode & S_ISUID)) {
    SPC_Format_Log((XeString)"+++> FAILURE: authentication file '%s' does not have the setuid bit set.",
		   tmpfile);
    REFUSE_REGISTRATION(SPC_Bad_Permission);
  }

  unlink(tmpfile);

  /*
   * Is uid associated with the authentication file created by the
   * client present in our password file?
   */
  if(!(pwent=getpwuid(buf.st_uid))) {
    SPC_Format_Log((XeString)"+++> FAILURE: the authentication file created by the client has a uid '%d'\n     and this uid is not in the password file.",
		   (int)buf.st_uid);
    REFUSE_REGISTRATION(SPC_Bad_Username);
  }

  /*
   * Comapre the user name in the request with the user name in
   * the passwd file.  They must be the same to continue.
   */
  if (strcmp (pwent->pw_name, username) != 0) {
    SPC_Format_Log((XeString)"+++> FAILURE: the request is for username '%s' with uid '%d'\n     but this uid has name '%s' in the password file.",
		   username,
		   (int)buf.st_uid,
		   pwent->pw_name);
    REFUSE_REGISTRATION(SPC_Bad_Password);
  }

  uid = pwent->pw_uid;
  gid = pwent->pw_gid;

  /*
   * Prepare the user's environment before giving up privileges.  The passwd
   * entry is static storage, so the strings are copied out of it here.
   */
  if(snprintf(HomeDir, sizeof(HomeDir), "HOME=%s", pwent->pw_dir) >= (int)sizeof(HomeDir) ||
     snprintf(ShellDir, sizeof(ShellDir), "SHELL=%s", pwent->pw_shell) >= (int)sizeof(ShellDir)) {
    SPC_Format_Log((XeString)"+++> FAILURE: the home directory or shell of '%s' is too long.", username);
    REFUSE_REGISTRATION(SPC_Env_Too_Big);
  }
  buffsize = strlen(pwent->pw_dir) + strlen(SPCD_ENV_HOME_DIRECTORY) +
	     strlen(SPCD_ENV_FILE) + 3;
  env_file = (XeString)malloc(buffsize);
  if(!env_file) {
    SPC_Format_Log((XeString)"+++> FAILURE: cannot malloc.");
    REFUSE_REGISTRATION(SPC_Out_Of_Memory);
  }
  snprintf(env_file, buffsize, "%s/%s/%s", pwent->pw_dir,
	   SPCD_ENV_HOME_DIRECTORY, SPCD_ENV_FILE);

  /*
   * We have authenticated ourselves.  Set the process identifiers of this
   * process to the looked up ones.  If that does not work completely the
   * daemon must not go on: it may still be root.
   */
  if(!Become_User(username, uid, gid)) {
    SPC_Write_Protocol_Request(client_connection, NULL, LOGFILE_REPLY,
			       FAILED_FILE_NAME, NULL, NULL);
    SPC_Error(SPC_Bad_Permission);
    SPCD_Exit(1);
  }
  client_validated=TRUE;

  Xechdir(HomeDir + strlen("HOME="));
  putenv(HomeDir);
  putenv(ShellDir);

  spc_user_environment_file = env_file;
  env_file = NULL;			/* kept for the life of the daemon */
  default_environment=
    SPC_Add_Env_File(spc_user_environment_file, default_environment);

  SPC_Write_Protocol_Request(client_connection, NULL, LOGFILE_REPLY,
			     PASSED_FILE_NAME, NULL, NULL);

  result = TRUE;

done:
  if(free_tmppath)
    XeFree(tmppath);
  if(free_netfile)
    tt_free(netfile);
  XeFree(tmpfile);
  XeFree(env_file);
  XeFree(username);
  XeFree(passwd);
  XeFree(proto_ver);
  XeFree(hostinfo);
  return(result);
}
#undef REFUSE_REGISTRATION

/*----------------------------------------------------------------------+*/
int Client_Unregister(protocol_request_ptr prot)
/*----------------------------------------------------------------------+*/
{
  return(print_protocol_request((XeString)"--> UNREGISTER", prot));
}

/*----------------------------------------------------------------------+*/
int Client_Channel_Open(protocol_request_ptr prot)
/*----------------------------------------------------------------------+*/
{
  int iomode = 0;
  SPC_Channel_Ptr channel;

  print_protocol_request((XeString)"--> CHANNEL_OPEN", prot);
  if(READ_OPEN(prot->dataptr, iomode) != 1) {
    SPC_Error(SPC_Protocol);
    return(SPC_ERROR);
  }

  /* Don't ever wanna wait, do line-oriented reads,
     use the toolkit, or execute as a system command */

  /* We don't do system commands because the client side has
     already converted the channel into the proper form for us */

  iomode &= ~(SPCIO_WAIT | SPCIO_LINEORIENTED |
	      SPCIO_USE_XTOOLKIT | SPCIO_SYSTEM);

  /* However, we do always want our termination to be synchronous */

  iomode |= SPCIO_SYNC_TERMINATOR;

  channel=XeSPCOpen(NULL, iomode);
  if(channel==SPC_ERROR)
    return(SPC_ERROR);

  XeSPCAddInput(channel, SPCD_Handle_Application_Data, channel);
  XeSPCRegisterTerminator(channel, SPCD_Termination_Handler, channel);

  /* The client names the channel by this id in its later requests. */
  return(channel->cid);
}

/*----------------------------------------------------------------------+*/
int Client_Channel_Close(protocol_request_ptr prot)
/*----------------------------------------------------------------------+*/
{
  SPC_Channel_Ptr channel=prot->channel;

  print_protocol_request((XeString)"--> CHANNEL_CLOSE", prot);

  if(IS_ACTIVE(channel)) {

    /*
      Uh-oh.  We have received a close request, but the channel is
      active.  We kill the process, and explicitly wait for the
      process to terminate.
      */

    XeSPCSignalProcess(channel, SIGKILL);
    SPC_Wait_For_Termination(channel);
  }
  
  return(XeSPCClose(channel));
}

/*----------------------------------------------------------------------+*/
int Client_Channel_Reset(protocol_request_ptr prot)
/*----------------------------------------------------------------------+*/
{
  SPC_Channel_Ptr channel=prot->channel;

  print_protocol_request((XeString)"--> CHANNEL_RESET", prot);

  return(XeSPCReset(channel));
}

/*----------------------------------------------------------------------+*/
int Client_Channel_Attach(protocol_request_ptr prot)
/*----------------------------------------------------------------------+*/
{
  SPC_Channel_Ptr channel=prot->channel;
  int pid = 0;

  print_protocol_request((XeString)"--> ATTACH", prot);

  if(READ_ATTACH(prot->dataptr, pid) != 1) {
    SPC_Error(SPC_Protocol);
    return(SPC_ERROR);
  }
  return(XeSPCAttach(channel, pid));
}

static void
Merge_Lang_Var(SPC_Channel_Ptr channel)
{
  XeString stdLang;

  if ((stdLang = SPC_Getenv((XeString)"LANG", channel->envp))
      != (XeString)NULL)
  {
    _DtXlateDb db = NULL;
    char platform[_DtPLATFORM_MAX_LEN];
    XeChar *langBuf;
    int execVer;
    int compVer;
    XeString myLang;

    if (_DtLcxOpenAllDbs(&db) == 0)
    {
      if ((_DtXlateGetXlateEnv(db, platform, &execVer, &compVer) == 0) &&
	  (_DtLcxXlateStdToOp(db, platform, compVer, DtLCX_OPER_SETLOCALE,
			      stdLang, NULL, NULL, NULL, &myLang) == 0))
      {
	if ((langBuf = (XeChar *)malloc((strlen(myLang) + 6)
					* sizeof(XeChar)))
	    != (XeChar *)NULL)
	{
	  sprintf(langBuf, "LANG=%s", myLang);
	  channel->envp = SPC_Putenv(langBuf, channel->envp);
	  free(langBuf);
	}

	free(myLang);
      }

      _DtLcxCloseDb(&db);
    }
  }
}

/*----------------------------------------------------------------------+*/
int Client_Application_Spawn(protocol_request_ptr prot)
/*----------------------------------------------------------------------+*/
{
  SPC_Channel_Ptr channel=prot->channel;
  int retval;

  print_protocol_request((XeString)"--> APPLICATION_SPAWN", prot);
  if(READ_APPLICATION_SPAWN(prot->dataptr,
			    channel->path,
			    channel->context_dir,
			    channel->argv, channel->envp) != TRUE) {
    SPC_Error(SPC_Protocol);
    return(SPC_ERROR);
  }

  Merge_Lang_Var(channel);

  channel->IOMode |= SPCIO_DEALLOC_ARGV;

  channel->envp=SPC_Merge_Envp(channel->envp, default_environment);

  retval=XeSPCExecuteProcess(channel);
  if(retval==SPC_ERROR)
    return(SPC_ERROR);

  return(channel->pid);
}

/*----------------------------------------------------------------------+*/
int Client_Application_Signal(protocol_request_ptr prot)
/*----------------------------------------------------------------------+*/
{
  XeString signame;
  int sig = 0;
  SPC_Channel_Ptr channel=prot->channel;

  print_protocol_request((XeString)"--> APPLICATION_SIGNAL", prot);
  READ_STRING_NO_COPY(prot->dataptr, signame);

  if (client_connection->protocol_version >= 2) {
      if ( (sig = XeNameToSignal( signame )) == XE_SIG_NOT_IN_TABLE ) {
	  SPC_Error(SPC_Bad_Signal_Name, signame);
	  return (SPC_ERROR);
      }
  } else {
      /* Must be the old 1.0, 1.1 protocol, assume its a number.       */
      /* Note however, that this is not portable as the signal numbers */
      /* differ from system to system.  This should really be an error */

      /* 1.0 code allowed signal 0 (check if process alive), but its     */
      /* part of XPG3 so we don't honor it .. nobody ever used it anyway */

      int ok = sscanf(signame, "%x", &sig);  /* WRITE_INT (%x) was used */
      if ( (ok == EOF) || (sig == 0) ) {
	  SPC_Error(SPC_Bad_Signal_Format, signame);
	  return (SPC_ERROR);
      }
  }

  return(XeSPCSignalProcess(channel, sig));
}

/*----------------------------------------------------------------------+*/
int Client_Application_Data(protocol_request_ptr prot)
/*----------------------------------------------------------------------+*/
{
  SPC_Channel_Ptr channel=prot->channel;
  buffered_data_ptr pdata=prot->dataptr;

  print_protocol_request((XeString)"--> APPLICATION_DATA", prot);

  return(XeSPCWrite(channel,
		    pdata->data+REQUEST_HEADER_LENGTH,
		    pdata->len));
}

/*----------------------------------------------------------------------+*/
int Client_Server_Debug(protocol_request_ptr prot)
/*----------------------------------------------------------------------+*/
{
  buffered_data_ptr pdata=prot->dataptr;
  XeString filename;
  time_t timeval;

  print_protocol_request((XeString)"--> SERVER_DEBUG", prot);
  /* The file name stays in the request buffer, which ends in a NUL. */
  READ_STRING_NO_COPY(pdata, filename);

  if(SPC_Print_Protocol)
    fclose(SPC_Print_Protocol);

  SPC_Print_Protocol=fopen(filename, (XeString)"a+");

  if(!SPC_Print_Protocol)
    return(ERROR);

  setbuf(SPC_Print_Protocol, NULL);

  time(&timeval);
  fprintf(SPC_Print_Protocol, (XeString)"Begin protocol filedump: %s", ctime(&timeval));
  return(TRUE);
}

/*
 ***
 *** The purpose of this routine is to filter out environment variables
 *** which should not get put into the environment.
 ***
*/

/*----------------------------------------------------------------------+*/
int conditional_putenv(XeString env_str)
/*----------------------------------------------------------------------+*/
{
  /* Returns TRUE when the string was handed to putenv(), which keeps it. */
  if(strncmp(env_str, (XeString)"HOME=", 5) && /* HOME may be different */
     strncmp(env_str, (XeString)"PWD=", 4)     /* PWD set by chdir, may change */
                                               /* Should SHELL be on this list? */
     )
    {
      return(putenv(env_str) == 0);
    }
  return(FALSE);
}

/*----------------------------------------------------------------------+*/
int Client_Environ_Reset(protocol_request_ptr prot)
/*----------------------------------------------------------------------+*/
{
  char **ret;
  int outlen, i;

  ret = SPC_Get_Multi_Packet(client_connection, prot,
			     &outlen,
			     ENVIRON_RESET, (XeString)"--> ENVIRON_RESET");

  if(ret == NULL)
    return(SPC_ERROR);

  /* putenv() keeps the strings it is given; the others are released. */
  for(i=0; i<outlen; i++)
    if(ret[i] && !(*ret[i] && conditional_putenv(ret[i])))
      free(ret[i]);

  free((char *)ret);

  return(TRUE);
}

/*----------------------------------------------------------------------+*/
int Client_Reply_Devices(protocol_request_ptr prot)
/*----------------------------------------------------------------------+*/
{
  SPC_Channel_Ptr channel=prot->channel;

  print_protocol_request((XeString)"--> QUERY_DEVICES", prot);

  SPC_Write_Protocol_Request(client_connection, channel, DEVICE_REPLY,
			     channel->wires[STDIN]->master_name,
			     channel->wires[STDIN]->slave_name,
			     channel->wires[STDOUT]->master_name,
			     channel->wires[STDOUT]->slave_name,
			     channel->wires[STDERR]->master_name,
			     channel->wires[STDERR]->slave_name);
  return(TRUE);
}

/*----------------------------------------------------------------------+*/
int Client_Reply_Logfile(protocol_request_ptr prot)
/*----------------------------------------------------------------------+*/
{
  char *netfile;
  SPC_Channel_Ptr channel=prot->channel;

  print_protocol_request((XeString)"--> QUERY_LOGFILE", prot);

  if (SPC_client_version_number >= SPC_PROTOCOL_VERSION_CDE_BASE && 
      IS_SPCIO_USE_LOGFILE(channel->IOMode)) {
    netfile = tt_file_netfile (channel->logfile);

    if (tt_ptr_error (netfile) != TT_OK) {
      SPC_Format_Log("+++> FAILURE: cannot create a 'netfile' name for the logfile.\n     (%s)", 
	             tt_status_message(tt_pointer_error(netfile)));
      return(SPC_ERROR);
    }

    SPC_Write_Protocol_Request(client_connection, channel, LOGFILE_REPLY,
			       netfile, NULL, NULL);
    tt_free (netfile);
  }
  else
    SPC_Write_Protocol_Request(client_connection, channel, LOGFILE_REPLY,
			       channel->logfile, NULL, NULL);

  return(TRUE);
}

/*----------------------------------------------------------------------+*/
int Client_Delete_Logfile(protocol_request_ptr prot)
/*----------------------------------------------------------------------+*/
{
  SPC_Channel_Ptr channel=prot->channel;

  print_protocol_request((XeString)"--> DELETE_LOGFILE", prot);

  return(XeSPCRemoveLogfile(channel));
}

/*----------------------------------------------------------------------+*/
int Client_Reset_Termios(protocol_request_ptr prot)
/*----------------------------------------------------------------------+*/
{
  print_protocol_request((XeString)"--> RESET_TERMIOS", prot);

  return(SPC_Get_Termios(prot));
}

/* New B.00 methods */

/*----------------------------------------------------------------------+*/
int Client_Send_EOF(protocol_request_ptr prot)
/*----------------------------------------------------------------------+*/
{
  SPC_Channel_Ptr channel=prot->channel;

  print_protocol_request((XeString)"--> SEND_EOF", prot);

  return(XeSPCSendEOF(channel));
}

int Client_Channel_Termios(protocol_request_ptr prot)
{
  SPC_Channel_Ptr channel=prot->channel;

  int connection, side;
  char buffer[1024];
  struct termios t;

  print_protocol_request((XeString)"--> CHANNEL_TERMIOS", prot);

  if(!SPC_Read_Termios_Request(prot, &connection, &side,
			       buffer, sizeof(buffer))) {
    SPC_Error(SPC_Protocol);
    return(SPC_ERROR);
  }
  memset(&t, 0, sizeof(t));
  SPC_Encode_Termios(buffer, &t);

  return(XeSPCSetTermio(channel, connection, side, &t));
}

int Client_Enhanced_Spawn(protocol_request_ptr prot)
{
  SPC_Channel_Ptr channel = prot->channel;
  char **ret;
  char *end;
  long numarg, numenv;
  int outlen, i;
  int argc_count, separator, first_env, env_count;
  int return_value;

  ret = SPC_Get_Multi_Packet(client_connection, prot,
			     &outlen,
			     APP_B00_SPAWN, (XeString)"--> APP_B00_SPAWN");

  if(ret == NULL)
    return(SPC_ERROR);

  /*
   * The strings are: path, directory, the counts of the argv and envp
   * strings (each one more than the strings sent, or zero when there is
   * none), the argv strings, a separator, and the environment strings.
   */
  if(outlen < 5 || !ret[0] || !ret[1] || !ret[2] || !ret[3])
    goto malformed;
  numarg = strtol(ret[2], &end, 10);
  if(ret[2][0] == '\0' || *end != '\0' || numarg < 0 || numarg > outlen)
    goto malformed;
  numenv = strtol(ret[3], &end, 10);
  if(ret[3][0] == '\0' || *end != '\0' || numenv < 0)
    goto malformed;

  argc_count = numarg > 0 ? (int)numarg - 1 : 0;
  separator = 4 + argc_count;
  if(separator >= outlen)
    goto malformed;
  for(i=4; i<separator; i++)
    if(!ret[i])
      goto malformed;
  first_env = separator + 1;
  env_count = outlen - first_env;

  channel->path = strdup(ret[0]);
  channel->context_dir  = strdup(ret[1]);
  if(!channel->path || !channel->context_dir) {
    SPC_Error(SPC_Out_Of_Memory);
    goto failed;
  }

  if(numarg == 0)
    channel->argv = NULL;
  else {
    channel->argv = &ret[4];
    free(ret[separator]);
    ret[separator] = NULL;		/* terminates argv */
  }

  if(numenv == 0)
    channel->envp = NULL;
  else {
    channel->envp = (char **)XeMalloc(((size_t)env_count+1)*sizeof(char *));
    channel->envp[0] = NULL;
    channel->envp = SPC_Merge_Envp(channel->envp, &ret[first_env]);

    Merge_Lang_Var(channel);

    channel->envp = SPC_Merge_Envp(channel->envp, default_environment);
  }

  return_value = XeSPCExecuteProcess(channel);

  /* Make the world safe for freeing the channel */

  channel->argv = NULL;
  if(channel->envp == &ret[first_env])
    channel->envp = NULL;

  for(i=0; i<outlen; i++)
    if(ret[i])
      free(ret[i]);
  free((char *)ret);

  if(return_value==SPC_ERROR)
    return(SPC_ERROR);

  return(channel->pid);

malformed:
  SPC_Error(SPC_Protocol);
failed:
  for(i=0; i<outlen; i++)
    free(ret[i]);
  free((char *)ret);
  return(SPC_ERROR);
}

/*----------------------------------------------------------------------+*/
void SPCD_Reply(SPC_Connection_Ptr connection,
                 protocol_request_ptr prot,
                 int retval,
                 int errval)
/*----------------------------------------------------------------------+*/
{
  if(retval==SPC_ERROR)
    retval= (-XeSPCErrorNumber);

  SPC_Write_Reply(connection, prot, retval, errval);
}

/*----------------------------------------------------------------------+*/
  /*
   * This function is invoked when the exit timer expires.
   * If sub-processes are running, return to select; 
   * otherwise exit.
   */
#ifdef SA_HANDLER_INT_ARG
void SPCD_Alarm_Handler(int not_used)
#else
void SPCD_Alarm_Handler()
#endif /* SA_HANDLER_INT_ARG */
/*----------------------------------------------------------------------+*/
{
  int i;

  if (exit_timeout == SPCD_NO_TIMER) 
    return;

  if (SPC_pid_list != NULL) 
    for (i=0; SPC_pid_list[i] != 0; i++) {
      if (SPC_pid_list[i] != SPCD_DEAD_PROCESS) {
	/*
	 * Have at least one sub- process running so reset the 
	 * alarm and return to select.
	 */
        (void) alarm (exit_timeout * 60);
	return;
      }
    }

  /*
   * There are no sub-processes running.  Exit if a request is not pending
   */
  if (request_pending != SPCD_REQUEST_PENDING) {
    SPC_Format_Log((XeString)
	           "Exit timer expired after '%d' minutes of no activity.", 
		   exit_timeout);
    SPCD_Exit (0);
  }
}
