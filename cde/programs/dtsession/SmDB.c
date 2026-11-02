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
/* $TOG: SmDB.c /main/5 1998/07/23 18:11:31 mgreess $ */
/*
 * (c) Copyright 1996 Digital Equipment Corporation.
 * (c) Copyright 1996 Hewlett-Packard Company.
 * (c) Copyright 1996 International Business Machines Corp.
 * (c) Copyright 1996 Sun Microsystems, Inc.
 * (c) Copyright 1996 Novell, Inc. 
 * (c) Copyright 1996 FUJITSU LIMITED.
 * (c) Copyright 1996 Hitachi.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include <string.h>
#include <ctype.h>
#include "SmDB.h"

#define CLIENT_ID_SZ 20

typedef struct _ClientDBRec
{
    XrmDatabase xrmDB;          /* Xrm resource database. */
    Boolean openForInput;       /* True if input, False if output. */

    char *xrmDBFileName;        /* Filename, for openForInput == False. */
    char *tmpDBFileName;
    FILE *xrmDBFile;
    Boolean writeFailed;       /* Sticky: a failed record cannot be committed. */

    /* Below used if openForInput. */
    char **XSMPClients;         /* malloc'd array of XSMP client IDs. */
    int nXSMPClients;           /* number of XSMP clients. */
    int nextXSMPClientIndex;    /* index of next XSMP client to Get;
				 * also used as sequence # for Put. */

    char **proxyClients;        /* malloc'd array of Proxy client IDs. */
    int nProxyClients;          /* number of Proxy clients. */
    int nextProxyClientIndex;   /* index of next Proxy client to Get;
				 * also used as sequence # for Put. */
} ClientDBRec;

static Boolean xrmInitialized = False;
static XrmName XSMPName[2];
static XrmClass XSMPClass[2];
static XrmName proxyName[2];
static XrmClass proxyClass[2];
static XrmQuark anyQuark;

typedef struct {
    char **strings;
    size_t count;
} StringArrayContext;

/* Public strings */
char *versionStr = ".version";
char *dtsessionIDStr = ".dtsessionID";

/* Strings for XSMP clients. */
static char *clientStr = "!\nClient.%s";
static char *programStr = "%s.Program";
static char *cwdStr = "%s.Cwd";
static char *restartCmdStr = "%s.RestartCommand";
static char *cloneCmdStr = "%s.CloneCommand";
static char *discardCmdStr = "%s.DiscardCommand";
static char *environmentStr = "%s.Environment";
static char *restartHintStr = "%s.RestartHint";
static char *sessionIDStr = "%s.SessionID";

/* Strings for Proxy clients. */
static char *proxyStr = "!\nProxyClient.%s";
static char *commandStr = "%s.Command";

/* Common resources (XSMP and Proxy). */
static char *clientHostStr = "%s.ClientHost";
static char *screenNumStr = "%s.ScreenNum";

static void _initXrm(void);
static Bool _countProc(XrmDatabase *, XrmBindingList, XrmQuarkList,
		       XrmRepresentation *, XrmValue *, XPointer);
static Bool _fillClientIDProc(XrmDatabase *, XrmBindingList, XrmQuarkList,
			      XrmRepresentation *, XrmValue *, XPointer);
static Bool _fillStringArrayProc(XrmDatabase *, XrmBindingList, XrmQuarkList,
				 XrmRepresentation *, XrmValue *, XPointer);
static Boolean _getStringResource(XrmDatabase, char *,
				  char *, char **, char *);
static Boolean _getIntResource(XrmDatabase, char *, char *, int *, int);
static Boolean _getStringArrayResource(XrmDatabase, char *, char *,
				       char ***, char **);
static void _freeStringArray(char **);
static Boolean _putStringResource(ClientDBRec *, char *, char *, char *);
static Boolean _putIntResource(ClientDBRec *, char *, char *, int);
static Boolean _putStringArrayResource(ClientDBRec *, char *, char *, char **);

static void
_initXrm(void)
{
    if (!xrmInitialized)
    {
	XrmInitialize(); /* Just in case. */

	anyQuark = XrmStringToQuark("?");
	XSMPName[0] = proxyName[0] = anyQuark;
	XSMPClass[0] = XrmStringToClass("Client");
	proxyClass[0] = XrmStringToClass("ProxyClient");
	XSMPName[1] = XSMPClass[1] =
	    proxyName[1] = proxyClass[1] = NULLQUARK;
	xrmInitialized = True;
    }
}

static Bool
_countProc(XrmDatabase *clientDB, XrmBindingList bindingList,
	   XrmQuarkList quarkList, XrmRepresentation *reps,
	   XrmValue *value, XPointer uData)
{
    int *countP = (int *)uData;

    if (*countP < 0 || (unsigned int)*countP >= UINT_MAX / sizeof(char *) - 1)
    {
	*countP = -1;
	return True;
    }
    ++*countP;

    return FALSE;
}

static Bool
_fillClientIDProc(XrmDatabase *clientDB, XrmBindingList bindingList,
		  XrmQuarkList quarkList, XrmRepresentation *reps,
		  XrmValue *value, XPointer uData)
{
    char ***idListPtr = (char ***)uData;
    char **clientIDPtr = *idListPtr;

    if (!value->addr || !value->size ||
	memchr(value->addr, '\0', value->size) != value->addr + value->size - 1 ||
	!*(char *)value->addr || strpbrk(value->addr, ".*\\ \t\r\n:"))
	return True;
    *clientIDPtr = (char *)value->addr;
    (*idListPtr)++;

    return FALSE;
}

static Bool
_fillStringArrayProc(XrmDatabase *clientDB, XrmBindingList bindingList,
		     XrmQuarkList quarkList, XrmRepresentation *reps,
		     XrmValue *value, XPointer uData)
{
    StringArrayContext *context = (StringArrayContext *)uData;
    const char *indexStr;
    size_t index = 0;

    /* Our index into this string array is at quarkList[2]. */
    if ((indexStr = XrmQuarkToString(quarkList[2])) == (char *)NULL)
	return True;


    /* Writer indices are decimal and dense. Never use an unchecked resource
     * name as an array subscript, including aliases such as 0 and 00.
     */
    if (!*indexStr)
	return True;
    for (; *indexStr; ++indexStr)
    {
	unsigned int digit = (unsigned char)*indexStr - '0';
	if (digit > 9 || digit >= context->count ||
	    index > (context->count - 1 - digit) / 10)
	    return True;
	index = index * 10 + digit;
    }
    if (index >= context->count || context->strings[index] != NULL ||
	value->size == 0 || value->addr == NULL ||
	memchr(value->addr, '\0', value->size) != value->addr + value->size - 1)
	return True;
    if ((context->strings[index] = XtNewString((char *)value->addr)) == NULL)
	return True;

    return FALSE;
}

static char *
_resourceName(const char *format, const char *clientID)
{
    char *name;
    int length;
    /* IDs occupy exactly one Xrm component. Separators would change both the
     * resource being selected and the number of quarks written by Xrm.
     */
    if (!clientID || !*clientID || strpbrk(clientID, ".*\\ \t\r\n:"))
	return NULL;
    length = snprintf(NULL, 0, format, clientID);
    if (length < 0)
	return NULL;
    name = XtMalloc((unsigned int)length + 1);
    if (name)
	snprintf(name, (size_t)length + 1, format, clientID);
    return name;
}

static Boolean
_getStringResource(XrmDatabase xrmDB, char *fmtStr,
		   char *clientID, char **resourcePtr, char *defaultVal)
{
    char *resourceType;
    XrmValue resourceValue;
    char *name = _resourceName(fmtStr, clientID);
    Bool found;

    if (!name)
	return False;
    found = XrmGetResource(xrmDB, name, name, &resourceType, &resourceValue);
    XtFree(name);
    if (found)
    {
	if (!resourceType || strcmp(resourceType, "String") != 0 ||
	    !resourceValue.addr || !resourceValue.size ||
	    memchr(resourceValue.addr, '\0', resourceValue.size) !=
		resourceValue.addr + resourceValue.size - 1)
	    return False;
	if ((*resourcePtr = XtNewString(resourceValue.addr)) == (char *)NULL)
	    return False;

	return True;
    }

    *resourcePtr = defaultVal;
    return True;
}

static Boolean
_getIntResource(XrmDatabase xrmDB, char *fmtStr,
		char *clientID, int *resourcePtr, int defaultVal)
{
    char *text = NULL, *end;
    long value;
    Boolean valid;


    if (!_getStringResource(xrmDB, fmtStr, clientID, &text, NULL))
	return False;
    if (!text)
    {
	*resourcePtr = defaultVal;
	return True;
    }
    errno = 0;
    value = strtol(text, &end, 10);
    valid = end != text && errno != ERANGE && value >= INT_MIN && value <= INT_MAX;
    while (isspace((unsigned char)*end))
	++end;
    valid = valid && *end == '\0';
    XtFree(text);
    if (!valid)
	return False;
    *resourcePtr = (int)value;

    return True;
}

static Boolean
_getStringArrayResource(XrmDatabase xrmDB, char *fmtStr,
			char *clientID, char ***resourcePtr,
			char **defaultVal)
{
    XrmQuark resourceName[3];
    XrmQuark resourceClass[3];
    char *name = _resourceName(fmtStr, clientID);
    int nStrings;

    resourceName[0] = resourceName[1] = anyQuark;
    resourceName[2] = NULLQUARK;

    if (!name)
	return False;
    XrmStringToQuarkList(name, resourceClass);
    XtFree(name);

    nStrings = 0;

    if (XrmEnumerateDatabase(xrmDB, resourceName, resourceClass,
			 XrmEnumOneLevel, _countProc,
			 (XPointer)&nStrings) || nStrings < 0)
	return False;

    if (nStrings > 0)
    {
	char **stringsPtr;
	int i;
	StringArrayContext context;

	if ((stringsPtr = (char **)XtMalloc((nStrings + 1) * sizeof(char *)))
	    == (char **)NULL)
	    return False;

	/* Initialize array entries to NULL so free can work. */
	/* NOTE: Final entry (nStrings) will remain NULL. */
	for (i = 0; i <= nStrings; i++)
	    stringsPtr[i] = (char *)NULL;

	context.strings = stringsPtr;
	context.count = nStrings;
	if (XrmEnumerateDatabase(xrmDB,
				 resourceName, resourceClass,
				 XrmEnumOneLevel, _fillStringArrayProc,
				 (XPointer)&context))
	{
	    for (i = 0; i < nStrings; i++)
		XtFree(stringsPtr[i]);
	    XtFree((char *)stringsPtr);
	    return False;
	}
	for (i = 0; i < nStrings; ++i)
	    if (!stringsPtr[i])
	    {
		int j;
		for (j = 0; j < nStrings; ++j)
		    XtFree(stringsPtr[j]);
		XtFree((char *)stringsPtr);
		return False;
	    }

	*resourcePtr = stringsPtr;
	return True;
    }

    *resourcePtr = defaultVal;
    return True;
}

static void
_freeStringArray(char **stringsPtr)
{
    if (stringsPtr)
    {
	int i;

	for (i = 0; stringsPtr[i] != (char *)NULL; i++)
	    XtFree(stringsPtr[i]);

	XtFree((char *)stringsPtr);
    }
}

static Boolean
_putStringResource(ClientDBRec *clientDB, char *fmtStr, char *clientID,
		   char *resourceVal)
{
    if (resourceVal != (char *)NULL)
    {
        char *name = _resourceName(fmtStr, clientID);
	int result;
	if (!name)
	    return False;
	result = fprintf(clientDB->xrmDBFile, "%s: %s\n", name, resourceVal);
	XtFree(name);
	if (result < 0)
	    return False;
    }

    return True;
}

static Boolean
_putIntResource(ClientDBRec *clientDB, char *fmtStr, char *clientID,
		int resourceVal)
{
    char *name = _resourceName(fmtStr, clientID);
    int result;
    if (!name)
	return False;
    result = fprintf(clientDB->xrmDBFile, "%s: %d\n", name, resourceVal);
    XtFree(name);
    if (result < 0)
	return False;

    return True;
}

static Boolean
_putStringArrayResource(ClientDBRec *clientDB, char *fmtStr, char *clientID,
			char **resourceVal)
{
    size_t i;
    char *name;

    if (resourceVal != (char **)NULL)
    {
	name = _resourceName(fmtStr, clientID);
	if (!name)
	    return False;
	for (i = 0; resourceVal[i] != (char *)NULL; i++)
	{
	    if (fprintf(clientDB->xrmDBFile, "%s.%zu: %s\n",
			name, i, resourceVal[i]) < 0)
	    {
		XtFree(name);
		return False;
	    }
	}
	XtFree(name);
    }

    return True;
}

ClientDB
OpenInputClientDB(char *fileName,
		  char **version,
		  char **dtsessionID)
{
    ClientDBRec *inputDB;
    char **tmpPtr;
    char *resourceType;
    XrmValue resourceValue;

    *version = (char *)NULL;
    *dtsessionID = (char *)NULL;

    _initXrm();

    if ((fileName == (char *)NULL) ||
	((inputDB = (ClientDBRec *)XtMalloc(sizeof(ClientDBRec)))
	 == (ClientDBRec *)NULL))
	return (ClientDB)NULL;

    if ((inputDB->xrmDB = XrmGetFileDatabase(fileName))
	== (XrmDatabase)NULL)
    {
	XtFree((char *)inputDB);
	return (ClientDB)NULL;
    }

    inputDB->openForInput = True;
    inputDB->XSMPClients = inputDB->proxyClients = (char **)NULL;
    inputDB->nXSMPClients = inputDB->nProxyClients = 0;
    inputDB->nextXSMPClientIndex = inputDB->nextProxyClientIndex = 0;

    /* Count the number of XSMP and Proxy clients. */
    if (XrmEnumerateDatabase(inputDB->xrmDB, XSMPName, XSMPClass,
			 XrmEnumOneLevel, _countProc,
			 (XPointer)&inputDB->nXSMPClients) ||
	XrmEnumerateDatabase(inputDB->xrmDB, proxyName, proxyClass,
			 XrmEnumOneLevel, _countProc,
			 (XPointer)&inputDB->nProxyClients))
    {
	XrmDestroyDatabase(inputDB->xrmDB);
	XtFree((char *)inputDB);
	return (ClientDB)NULL;
    }

    /* Allocate space for the client IDs and fill from database. */
    if (inputDB->nXSMPClients > 0)
    {
	if ((inputDB->XSMPClients =
	     (char **)XtMalloc(inputDB->nXSMPClients * sizeof(char *)))
	    == (char **)NULL)
	{
	    XrmDestroyDatabase(inputDB->xrmDB);
	    XtFree((char *)inputDB);
	    return (ClientDB)NULL;
	}

	tmpPtr = inputDB->XSMPClients;
	if (XrmEnumerateDatabase(inputDB->xrmDB, XSMPName, XSMPClass,
			     XrmEnumOneLevel, _fillClientIDProc,
			     (XPointer)&tmpPtr))
	{
	    XrmDestroyDatabase(inputDB->xrmDB);
	    XtFree((char *)inputDB->XSMPClients);
	    XtFree((char *)inputDB);
	    return (ClientDB)NULL;
	}
    }
    if (inputDB->nProxyClients > 0)
    {
	if ((inputDB->proxyClients =
	     (char **)XtMalloc(inputDB->nProxyClients * sizeof(char *)))
	    == (char **)NULL)
	{
	    XrmDestroyDatabase(inputDB->xrmDB);
	    XtFree((char *)inputDB->XSMPClients);
	    XtFree((char *)inputDB);
	    return (ClientDB)NULL;
	}

	tmpPtr = inputDB->proxyClients;
	if (XrmEnumerateDatabase(inputDB->xrmDB, proxyName, proxyClass,
			     XrmEnumOneLevel, _fillClientIDProc,
			     (XPointer)&tmpPtr))
	{
	    XrmDestroyDatabase(inputDB->xrmDB);
	    XtFree((char *)inputDB->XSMPClients);
	    XtFree((char *)inputDB->proxyClients);
	    XtFree((char *)inputDB);
	    return (ClientDB)NULL;
	}
    }

    if ((!XrmGetResource(inputDB->xrmDB, versionStr, versionStr,
			 &resourceType, &resourceValue)) ||
	((*version = XtNewString(resourceValue.addr)) == (char *)NULL) ||
	(!XrmGetResource(inputDB->xrmDB, dtsessionIDStr, dtsessionIDStr,
			 &resourceType, &resourceValue)) ||
	((*dtsessionID = XtNewString(resourceValue.addr)) == (char *)NULL))
    {
	if (*version)
	{
	    XtFree(*version);
	    *version = (char *)NULL;
	}
	XrmDestroyDatabase(inputDB->xrmDB);
	XtFree((char *)inputDB->XSMPClients);
	XtFree((char *)inputDB->proxyClients);
	XtFree((char *)inputDB);
	return (ClientDB)NULL;
    }

    return (ClientDB)inputDB;
}

ClientDB
OpenOutputClientDB(char *fileName,
		   char *version,
		   char *dtsessionID)
{
    ClientDBRec *outputDB;
    size_t length;
    int fd, savedErrno;
    const char suffix[] = ".XXXXXX";

    _initXrm();

    if ((fileName == (char *)NULL) ||
	(version == (char *)NULL) ||
	(dtsessionID == (char *)NULL) ||
	((outputDB = (ClientDBRec *)XtCalloc(1, sizeof(ClientDBRec)))
	 == (ClientDBRec *)NULL))
	return (ClientDB)NULL;

    outputDB->xrmDB = (XrmDatabase)NULL;
    outputDB->openForInput = False;
    outputDB->nextXSMPClientIndex = 0;
    outputDB->nextProxyClientIndex = 0;

    /* Keep the previous database in place until the replacement is complete.
     * Xt allocation sizes are Cardinal, so check before narrowing.
     */
    length = strlen(fileName);
    if (length > UINT_MAX - sizeof(suffix))
    {
	XtFree((char *)outputDB);
	errno = ENAMETOOLONG;
	return (ClientDB)NULL;
    }
    if (((outputDB->xrmDBFileName = XtNewString(fileName)) == (char *)NULL) ||
	((outputDB->tmpDBFileName = XtMalloc(length + sizeof(suffix))) == NULL))
    {
	XtFree(outputDB->xrmDBFileName);
	XtFree((char *)outputDB);
	return (ClientDB)NULL;
    }

    memcpy(outputDB->tmpDBFileName, fileName, length);
    memcpy(outputDB->tmpDBFileName + length, suffix, sizeof(suffix));
    fd = mkstemp(outputDB->tmpDBFileName);
    if (fd < 0)
    {
	XtFree(outputDB->xrmDBFileName);
	XtFree(outputDB->tmpDBFileName);
	XtFree((char *)outputDB);
	return (ClientDB)NULL;
    }

    if ((outputDB->xrmDBFile = fdopen(fd, "w")) == NULL)
    {
	savedErrno = errno;
	close(fd);
	unlink(outputDB->tmpDBFileName);
	XtFree(outputDB->xrmDBFileName);
	XtFree(outputDB->tmpDBFileName);
	XtFree((char *)outputDB);
	errno = savedErrno;
	return (ClientDB)NULL;
    }

    /* Store version and session ID. */
    if (fprintf(outputDB->xrmDBFile, "! dtsession.db\n!\n%s: %s\n",
		versionStr, version) < 0 ||
	fprintf(outputDB->xrmDBFile, "%s: %s\n", dtsessionIDStr, dtsessionID) < 0)
    {
	savedErrno = errno;
	CloseClientDB((ClientDB)outputDB, False);
	errno = savedErrno;
	return (ClientDB)NULL;
    }

    return (ClientDB)outputDB;
}

XSMPClientDBRecPtr
GetXSMPClientDBRec(ClientDB inputDBPtr)
{
    ClientDBRec *inputDB = (ClientDBRec *)inputDBPtr;
    XSMPClientDBRecPtr clientPtr;
    char *clientID;

    if ((inputDB == (ClientDBRec *)NULL) ||
	(!inputDB->openForInput) ||
	(inputDB->nextXSMPClientIndex >= inputDB->nXSMPClients) ||
	((clientPtr = (XSMPClientDBRecPtr)XtMalloc(sizeof(XSMPClientDBRec)))
	 == (XSMPClientDBRecPtr)NULL))
    {
	return (XSMPClientDBRecPtr)NULL;
    }

    clientID = inputDB->XSMPClients[inputDB->nextXSMPClientIndex];

    /* Initialize pointers so Free is easy. */
    clientPtr->clientId = (char *)NULL;
    clientPtr->clientHost = clientPtr->program =
	clientPtr->cwd = (char *)NULL;
    clientPtr->restartCommand = clientPtr->cloneCommand =
	clientPtr->discardCommand = clientPtr->environment = (char **)NULL;

    /* Retrieve resources from the database. */
    if (!_getStringResource(inputDB->xrmDB, sessionIDStr, clientID,
			    &clientPtr->clientId, (char *)NULL) ||
	!_getStringResource(inputDB->xrmDB, clientHostStr, clientID,
			     &clientPtr->clientHost, (char *)NULL) ||
	!_getStringResource(inputDB->xrmDB, programStr, clientID,
			    &clientPtr->program, (char *)NULL) ||
	!_getStringResource(inputDB->xrmDB, cwdStr, clientID,
			    &clientPtr->cwd, (char *)NULL) ||
	!_getIntResource(inputDB->xrmDB, screenNumStr, clientID,
			 &clientPtr->screenNum, 0) ||
	!_getStringArrayResource(inputDB->xrmDB, restartCmdStr, clientID,
				 &clientPtr->restartCommand, (char **)NULL) ||
	!_getStringArrayResource(inputDB->xrmDB, cloneCmdStr, clientID,
				 &clientPtr->cloneCommand, (char **)NULL) ||
	!_getStringArrayResource(inputDB->xrmDB, discardCmdStr, clientID,
				 &clientPtr->discardCommand, (char **)NULL) ||
	!_getStringArrayResource(inputDB->xrmDB, environmentStr, clientID,
				 &clientPtr->environment, (char **)NULL) ||
	!_getIntResource(inputDB->xrmDB, restartHintStr, clientID,
			 &clientPtr->restartHint, 0) ||
	clientPtr->screenNum < 0 || clientPtr->restartHint < SmRestartIfRunning ||
	clientPtr->restartHint > SmRestartNever)
    {
	FreeXSMPClientDBRec(clientPtr);
	return (XSMPClientDBRecPtr)NULL;
    }

    clientPtr->next = (XSMPClientDBRec *)NULL;

    inputDB->nextXSMPClientIndex++;

    return clientPtr;
}

ProxyClientDBRecPtr
GetProxyClientDBRec(ClientDB inputDBPtr)
{
    ClientDBRec *inputDB = (ClientDBRec *)inputDBPtr;
    ProxyClientDBRecPtr clientPtr;
    char *clientID;

    if ((inputDB == (ClientDBRec *)NULL) ||
	(!inputDB->openForInput) ||
	(inputDB->nextProxyClientIndex >= inputDB->nProxyClients) ||
	((clientPtr = (ProxyClientDBRecPtr)XtMalloc(sizeof(ProxyClientDBRec)))
	 == (ProxyClientDBRecPtr)NULL))
    {
	return (ProxyClientDBRecPtr)NULL;
    }

    clientID = inputDB->proxyClients[inputDB->nextProxyClientIndex];

    /* Initialize pointers so Free is easy. */
    clientPtr->clientHost = (char *)NULL;
    clientPtr->command = (char **)NULL;

    /* Retrieve resources from the database. */
    if (!_getStringResource(inputDB->xrmDB, clientHostStr, clientID,
			     &clientPtr->clientHost, (char *)NULL) ||
	!_getStringArrayResource(inputDB->xrmDB, commandStr, clientID,
				 &clientPtr->command, (char **)NULL) ||
	!_getIntResource(inputDB->xrmDB, screenNumStr, clientID,
			 &clientPtr->screenNum, 0) || clientPtr->screenNum < 0)
    {
	FreeProxyClientDBRec(clientPtr);
	return (ProxyClientDBRecPtr)NULL;
    }

    inputDB->nextProxyClientIndex++;

    return clientPtr;
}

Boolean
PutXSMPClientDBRec(ClientDB outputDBPtr,
		   XSMPClientDBRecPtr clientPtr)
{
    ClientDBRec *outputDB = (ClientDBRec *)outputDBPtr;
    char clientID[CLIENT_ID_SZ];

    if ((outputDB == (ClientDBRec *)NULL) || outputDB->openForInput)
	return False;
    if (outputDB->writeFailed ||
	outputDB->nextXSMPClientIndex == INT_MAX ||
	(clientPtr == (XSMPClientDBRecPtr)NULL) ||
	(clientPtr->clientId == (char *)NULL))

    {
	outputDB->writeFailed = True;
	return False;
    }

    snprintf(clientID, sizeof(clientID), "C%d", outputDB->nextXSMPClientIndex++);

    if (!_putStringResource(outputDB, clientStr, clientID, clientID) ||
	!_putStringResource(outputDB, sessionIDStr, clientID,
			    clientPtr->clientId) ||
	!_putStringResource(outputDB, clientHostStr, clientID,
			    clientPtr->clientHost) ||
	!_putStringResource(outputDB, programStr, clientID,
			    clientPtr->program) ||
	!_putStringResource(outputDB, cwdStr, clientID,
			    clientPtr->cwd) ||
	!_putIntResource(outputDB, screenNumStr, clientID,
			 clientPtr->screenNum) ||
	!_putStringArrayResource(outputDB, restartCmdStr, clientID,
				 clientPtr->restartCommand) ||
	!_putStringArrayResource(outputDB, cloneCmdStr, clientID,
				 clientPtr->cloneCommand) ||
	!_putStringArrayResource(outputDB, discardCmdStr, clientID,
				 clientPtr->discardCommand) ||
	!_putStringArrayResource(outputDB, environmentStr, clientID,
				 clientPtr->environment) ||
	!_putIntResource(outputDB, restartHintStr, clientID,
			 (int)clientPtr->restartHint))
    {
	outputDB->writeFailed = True;
	return False;
    }

    return True;
}

Boolean
PutProxyClientDBRec(ClientDB outputDBPtr,
		    ProxyClientDBRecPtr clientPtr)
{
    ClientDBRec *outputDB = (ClientDBRec *)outputDBPtr;
    char clientID[CLIENT_ID_SZ];

    if ((outputDB == (ClientDBRec *)NULL) || outputDB->openForInput)
	return False;
    if (outputDB->writeFailed ||
	outputDB->nextProxyClientIndex == INT_MAX ||
	(clientPtr == (ProxyClientDBRecPtr)NULL))
    {
	outputDB->writeFailed = True;
	return False;
    }

    snprintf(clientID, sizeof(clientID), "PC%d", outputDB->nextProxyClientIndex++);

    if (!_putStringResource(outputDB, proxyStr, clientID, clientID) ||
	!_putStringResource(outputDB, clientHostStr, clientID,
			    clientPtr->clientHost) ||
	!_putStringArrayResource(outputDB, commandStr, clientID,
				 clientPtr->command) ||
	!_putIntResource(outputDB, screenNumStr, clientID,
			 clientPtr->screenNum))
    {
	outputDB->writeFailed = True;
	return False;
    }

    return True;
}

Boolean
CloseClientDB(ClientDB clientDBPtr, Boolean writeDB)
{
    ClientDBRec *clientDB = (ClientDBRec *)clientDBPtr;
    Boolean result = True;
    int savedErrno = 0;

    if (clientDB == (ClientDBRec *)NULL)
	return False;

    /* Input? */
    if (clientDB->openForInput)
    {
	XtFree((char *)clientDB->XSMPClients);
	XtFree((char *)clientDB->proxyClients);
	XrmDestroyDatabase(clientDB->xrmDB);
	XtFree((char *)clientDB);

	return True;
    }

    /* A buffered write failure must never replace the previous database. */
    if (writeDB)
    {
	if (clientDB->writeFailed || ferror(clientDB->xrmDBFile) ||
	    fflush(clientDB->xrmDBFile) != 0 ||
	    fsync(fileno(clientDB->xrmDBFile)) != 0)
	{
	    result = False;
	    savedErrno = errno ? errno : EIO;
	}
    }
    if (fclose(clientDB->xrmDBFile) != 0 && result)
    {
	result = False;
	savedErrno = errno ? errno : EIO;
    }
    if (writeDB && result &&
	rename(clientDB->tmpDBFileName, clientDB->xrmDBFileName) != 0)
    {
	result = False;
	savedErrno = errno;
    }
    if (!writeDB || !result)
	unlink(clientDB->tmpDBFileName);

    XtFree(clientDB->xrmDBFileName);
    XtFree(clientDB->tmpDBFileName);
    XtFree((char *)clientDB);

    if (!result)
	errno = savedErrno;
    return result;
}

void
FreeXSMPClientDBRec(XSMPClientDBRecPtr clientPtr)
{
    if (clientPtr != (XSMPClientDBRecPtr)NULL)
    {
	XtFree(clientPtr->clientId);
	XtFree(clientPtr->clientHost);
	XtFree(clientPtr->program);
	XtFree(clientPtr->cwd);

	_freeStringArray(clientPtr->restartCommand);
	_freeStringArray(clientPtr->cloneCommand);
	_freeStringArray(clientPtr->discardCommand);
	_freeStringArray(clientPtr->environment);

	XtFree((char *)clientPtr);
    }
}

void
FreeProxyClientDBRec(ProxyClientDBRecPtr clientPtr)
{
    if (clientPtr != (ProxyClientDBRecPtr)NULL)
    {
	XtFree(clientPtr->clientHost);

	_freeStringArray(clientPtr->command);

	XtFree((char *)clientPtr);
    }
}
