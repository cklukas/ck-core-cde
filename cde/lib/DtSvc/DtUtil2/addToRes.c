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
/*                                                                            *
 * (c) Copyright 1993, 1994 Hewlett-Packard Company                           *
 * (c) Copyright 1993, 1994 International Business Machines Corp.             *
 * (c) Copyright 1993, 1994 Sun Microsystems, Inc.                            *
 * (c) Copyright 1993, 1994 Novell, Inc.                                      *
 */
/*Add a string to the XA_RESOURCE_MANAGER*/
/*
 *                        COPYRIGHT 1987
 *                 DIGITAL EQUIPMENT CORPORATION
 *                     MAYNARD, MASSACHUSETTS
 *                      ALL RIGHTS RESERVED.
 *
 * THE INFORMATION IN THIS SOFTWARE IS SUBJECT TO CHANGE WITHOUT NOTICE AND
 * SHOULD NOT BE CONSTRUED AS A COMMITMENT BY DIGITAL EQUIPMENT CORPORATION.
 * DIGITAL MAKES NO REPRESENTATIONS ABOUT THE SUITABILITY OF THIS SOFTWARE FOR
 * ANY PURPOSE.  IT IS SUPPLIED "AS IS" WITHOUT EXPRESS OR IMPLIED WARRANTY.
 *
 * IF THE SOFTWARE IS MODIFIED IN A MANNER CREATING DERIVATIVE COPYRIGHT RIGHTS,
 * APPROPRIATE LEGENDS MAY BE PLACED ON THE DERIVATIVE WORK IN ADDITION TO THAT
 * SET FORTH ABOVE.
 *
 *
 * Permission to use, copy, modify, and distribute this software and its
 * documentation for any purpose and without fee is hereby granted, provided
 * that the above copyright notice appear in all copies and that both that
 * copyright notice and this permission notice appear in supporting
 * documentation, and that the name of Digital Equipment Corporation not be
 * used in advertising or publicity pertaining to distribution of the software
 * without specific, written prior permission.
 */

/*Lifted from xrdb(1X)*/
/* -*-C-*-
*******************************************************************************
*
* File:         addToResource.c
* RCS:          $TOG: addToRes.c /main/8 1999/10/14 16:01:17 mgreess $
* Description:  Source code for adding strings to RESOURCE_PROPERTY on 
                  default root window
* Author:       DEC, Robert Williams
* Created:      Thu Apr 26 14:42:08 PDT 1990
* Modified:	Kim Dronesen
* Language:     C
* Package:      N/A
* Status:       Experimental (Do Not Distribute)
*
* (C) Copyright 1990, Hewlett-Packard, all rights reserved.
*
*******************************************************************************
*/
/*$TOG: addToRes.c /main/8 1999/10/14 16:01:17 mgreess $ */

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xos.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <Dt/DtP.h>
#include <Dt/SessionM.h>
#include "DtSvcLock.h"

/****************************************/
/* Global variables */
/****************************************/

typedef struct _Entry {
    char *tag, *value;
    int lineno;
    Bool usable;
} Entry;
typedef struct _Buffer {
    char *buff;
    int  room, used;
    Bool freebuff;
} Buffer;
typedef struct _Entries {
    Entry *entry;
    int   room, used;
} Entries;

#define INIT_BUFFER_SIZE 10000
#define INIT_ENTRY_SIZE 500


/********    Public Function Declarations    ********/

static Buffer * _DtAllocBuffer(const char **buff);
static void _DtFreeBuffer(Buffer *b);
static int _DtAppendToBuffer( 
                        Buffer *b,
                        char *str,
                        int len) ;
static int _DtAppendEntryToBuffer( 
                        Buffer *buffer,
                        Entry entry) ;

static Entries * _DtAllocEntries( void) ;
static  void _DtFreeEntries( Entries *) ;
static int _DtAddEntry( 
                        Entries *e,
                        Entry entry) ;
static int _DtCompareEntries( 
                        const void *e1,
                        const void *e2) ;
static char * _DtFindFirst( 
                        char *string,
                        char dest) ;
static int _DtGetEntries( 
                        Entries *entries,
                        Buffer *buff,
                        int dosort) ;
static int _DtMergeEntries( 
                        Buffer *buffer,
                        Entries new,
                        Entries old) ;
static void _DtAddToResProp(
                        Display *dpy,
                        unsigned int id,
                        Entries db) ;
static void _getWinProp(
                        Display *dpy,
                        unsigned int id,
                        Window *win,
                        Atom *prop);

/********    End Public Function Declarations    ********/

/****************************************/
/*The meat*/
/****************************************/

static Buffer *
_DtAllocBuffer(
	const char **buff )
{
    Buffer *b = (Buffer *)malloc(sizeof(Buffer));

    if (b == nullptr)
	return nullptr;
    b->room = INIT_BUFFER_SIZE;
    if (buff) {
	b->buff = (char*) *buff;
    } else {
	b->buff = (char *)malloc(INIT_BUFFER_SIZE*sizeof(char));
	if (b->buff == nullptr) {
	    free(b);
	    return nullptr;
	}
    }
    b->used = buff && *buff ? strlen(*buff) : 0;
    b->freebuff = buff ? False : True;
    return(b);
}

static void
_DtFreeBuffer(
	Buffer *b)
{
    if (b == nullptr) return;
    if (b->freebuff == True) free(b->buff);
    free(b);
}

static int
_DtAppendToBuffer(
	Buffer *b,
	char *str,
	int len )
{
    while (b->used + len > b->room) {
	char *grown;

	if (b->room > INT_MAX / 2)
	    return -1;
	grown = (char *)realloc(b->buff, 2*(size_t)b->room);
	if (grown == nullptr)
	    return -1;
	b->buff = grown;
	b->room *= 2;
    }
    strncpy(b->buff + b->used, str, len);
    b->used += len;
    return 0;
}

static Entries *
_DtAllocEntries( void )
{
    Entries *e = (Entries *)malloc(sizeof(Entries));

    if (e == nullptr)
	return nullptr;
    e->room = INIT_ENTRY_SIZE;
    e->used = 0;
    e->entry = (Entry *)calloc(INIT_ENTRY_SIZE, sizeof(Entry));
    if (e->entry == nullptr) {
	free(e);
	return nullptr;
    }
    return(e);
}

static void
_DtFreeEntries( 
    Entries *e)
{
    int n = 0;

    if (e == nullptr) return;
    while (n < e->used)
    {
	free(e->entry[n].tag);
	free(e->entry[n].value);
	n++;
    }
    free(e->entry);
    free(e);
}

/*
 * Adds the entry, which then belongs to the table.  Returns -1 when the
 * table cannot grow; the entry is released in that case.
 */
static int
_DtAddEntry(
	Entries *e,
	Entry entry )
{
    int n;

    for (n = 0; n < e->used; n++)
    {
	if (strcmp(e->entry[n].tag, entry.tag) == 0)
	{ /* overwrite old entry  - free its memory first*/
	    free(e->entry[n].tag);
	    free(e->entry[n].value);
	    e->entry[n] = entry;
	    return 0;  /* ok to leave, now there's only one of each tag in db */
	}
    }

    if (e->used == e->room) {
	Entry *grown;

	if (e->room > INT_MAX / 2)
	    grown = nullptr;
	else
	    grown = (Entry *)reallocarray(e->entry, 2*(size_t)e->room, sizeof(Entry));
	if (grown == nullptr) {
	    /* the table keeps its old, valid block */
	    free(entry.tag);
	    free(entry.value);
	    return -1;
	}
	e->entry = grown;
	e->room *= 2;
    }
    entry.usable = True;
    e->entry[e->used++] = entry;
    return 0;
}

static int 
_DtCompareEntries(
        const void *item1,
        const void *item2 )
{
    const Entry *e1 = item1;
    const Entry *e2 = item2;
    return strcmp(e1->tag, e2->tag);
}

static int 
_DtAppendEntryToBuffer(
	Buffer *buffer,
	Entry entry )
{
    if (_DtAppendToBuffer(buffer, entry.tag, strlen(entry.tag)) != 0 ||
	_DtAppendToBuffer(buffer, ":\t", 2) != 0 ||
	_DtAppendToBuffer(buffer, entry.value, strlen(entry.value)) != 0 ||
	_DtAppendToBuffer(buffer, "\n", 1) != 0)
	return -1;
    return 0;
}

static char * 
_DtFindFirst(
        char *string,
        char dest )
{
    int len;

    for (;;) {
        if((len = mblen(string, MB_CUR_MAX)) > 1) {
            string += len;
            continue;
        }
        if (*string == '\0')
            return nullptr;
        if (*string == '\\') {
            if (*++string == '\0')
                return nullptr;
        }
        else if (*string == dest)
            return string;
        string++;
    }
}

static int
_DtGetEntries(
        Entries *entries,
        Buffer *buff,
        int dosort )
{
    char *line, *colon, *temp, *str, *temp2;
    Entry entry = { nullptr, nullptr, 0, False };
    int length;
    int lineno = 0;

    str = buff->buff;
    if (!str) return 0;
    for (; str < buff->buff + buff->used; str = line + 1)
    {
        line = _DtFindFirst(str, '\n');
        lineno++;
        if (line == nullptr)
            break;
        if (str[0] == '!')
            continue;
        if (str[0] == '\n')
            continue;
        if (str[0] == '#')
	{
            int dummy;
            if (sscanf (str, "# %d", &dummy) == 1) lineno = dummy - 1;
            continue;
        }
        for (temp = str;
             *temp && *temp != '\n' && isascii(*temp) && isspace((u_char)*temp);
             temp++) ;
        if (!*temp || *temp == '\n') continue;

        colon = _DtFindFirst(str, ':');
        if (colon == nullptr)
            break;
        if (colon > line)
	{
            line[0] = '\0';
            fprintf (stderr,
                     "%s:  colon missing on line %d, ignoring entry \"%s\"\n",
                     "dtprefs", lineno, str);
            continue;
        }

	temp2 = str;
        while (temp2[0] == ' ' || temp2[0] == '\t')
	{
            temp2++;
        }
        temp = (char *)malloc((length = colon - temp2) + 1);
	if (temp == nullptr)
	    return -1;
        strncpy(temp, temp2, length);
        temp[length] = '\0';
        while (temp[length-1] == ' ' || temp[length-1] == '\t')
            temp[--length] = '\0';
        entry.tag = temp;
	

	temp2 = colon + 1;
        while (temp2[0] == ' ' || temp2[0] == '\t')
	{
            temp2++;
        }
        temp = (char *)malloc((length = line - temp2) + 1);
	if (temp == nullptr) {
	    free(entry.tag);
	    return -1;
	}
        strncpy(temp, temp2, length);
        temp[length] = '\0';
        entry.value = temp;
        entry.lineno = lineno;

	if (_DtAddEntry(entries, entry) != 0)
	    return -1;
    }

    if (dosort && (entries->used > 0))
      qsort(entries->entry, entries->used, sizeof(Entry),
     _DtCompareEntries);
    return 0;
}

static int 
_DtMergeEntries(
	Buffer *buffer,
	Entries new,
	Entries old )
{
    int n, o, cmp;

    buffer->used = 0;
    n = o = 0;
    while ((n < new.used) && (o < old.used))
    {
	cmp = strcmp(new.entry[n].tag, old.entry[o].tag);
	if (cmp > 0)
	{
	    if (_DtAppendEntryToBuffer(buffer, old.entry[o]) != 0)
		return -1;
	    o++;
	}
	else
	{
	    if (_DtAppendEntryToBuffer(buffer, new.entry[n]) != 0)
		return -1;
	    n++;
	    if (cmp == 0)
	    {
		o++;
	    }
	}
    }
    
    while (n < new.used)
    {
	if (_DtAppendEntryToBuffer(buffer, new.entry[n]) != 0)
	    return -1;
	n++;
    }
    while (o < old.used)
    {
	if (_DtAppendEntryToBuffer(buffer, old.entry[o]) != 0)
	    return -1;
	o++;
    }
    
    return _DtAppendToBuffer(buffer, "\0", 1);
}

static void
_getWinProp(
        Display *dpy,
        unsigned int id,
        Window *win,
        Atom *prop)
{
  static Bool init = True;
  static Window winprop;
  static Atom xa_resmgr;
  static Atom xa_prefs;

  _DtSvcProcessLock();
  if (init == True)
  {
    winprop = XRootWindow(dpy, 0);
    xa_resmgr = XA_RESOURCE_MANAGER;
    xa_prefs = XInternAtom (dpy, _XA_DT_SM_PREFERENCES, False);
    init = False;
  }
  _DtSvcProcessUnlock();

  *win = winprop;
  *prop = id == _DT_ATR_RESMGR ? xa_resmgr : xa_prefs;
}


static void 
_DtAddToResProp(
        Display *dpy,
        unsigned int id,
        Entries db)
{
    char *xdefs;
    Buffer *oldBuffer, *newBuffer = nullptr;
    Entries *oldDB;
    int                 defStatus;
    Atom                actualType;
    int                 actualFormat;
    unsigned long       nitems, leftover;
    Window              win;
    Atom                prop;

   /*
    * Get window and property
    */
    _getWinProp(dpy, id, &win, &prop);
    if (win == (Window)0)
    {
      return;
    }

   /*
    * Get resource database from specified window and property.
    */
    defStatus = XGetWindowProperty(dpy, win,
				   prop, 0L,
				   100000000L,False,XA_STRING,&actualType,
				   &actualFormat,&nitems,&leftover,
				   (unsigned char**) &xdefs);


   /*
    * Allocate oldBuffer and init from resource database string
    */
    oldBuffer = _DtAllocBuffer((const char**) &xdefs);

   /*
    * Allocate oldDB
    */
    oldDB = oldBuffer ? _DtAllocEntries() : nullptr;

   /*
    * Convert oldBuffer to oldDB, init empty newBuffer, then populate
    * by merging db into oldDB.
    */
    if (oldDB != nullptr && _DtGetEntries(oldDB, oldBuffer, 1) == 0)
    {
	newBuffer = _DtAllocBuffer(nullptr);
	if (newBuffer != nullptr &&
	    _DtMergeEntries(newBuffer, db, *oldDB) == 0)
	{
	   /*
	    * Finally, store newBuffer into resource database.
	    */
	    XChangeProperty (dpy, win, prop,
			     XA_STRING, 8, PropModeReplace,
			     (unsigned char *)newBuffer->buff, newBuffer->used);

	    XSync(dpy, False);
	}
    }

   /*
    * Free buffer memory
    */
    if (xdefs) XFree(xdefs);
    _DtFreeBuffer(oldBuffer);
    _DtFreeBuffer(newBuffer);
    _DtFreeEntries(oldDB);
}

char *
_DtGetResString(
        Display *dpy,
        unsigned int id)
{
    char *xdefs;
    Buffer *oldBuffer, *newBuffer;
    Entries *oldDB;
    int                 defStatus;
    Atom                actualType;
    int                 actualFormat;
    unsigned long       nitems, leftover;
    Window              win;
    Atom                prop;

   /*
    * Get window and property
    */
    _getWinProp(dpy, id, &win, &prop);
    if (win == (Window)0)
    {
      return nullptr;
    }

   /*
    * Get resource database from specified window and property.
    */
    defStatus = XGetWindowProperty(dpy, win,
				   prop, 0L,
				   100000000L,False,XA_STRING,&actualType,
				   &actualFormat,&nitems,&leftover,
				   (unsigned char**) &xdefs);

    return(xdefs);
}

void 
_DtAddToResource(
	Display *dpy,
	const char *data )
{
  _DtAddResString( dpy, data, _DT_ATR_RESMGR|_DT_ATR_PREFS);
}

void 
_DtAddResString(
        Display *dpy,
        const char *data, 
        unsigned int flags)
{
    char *xdefs;
    int i;
    Buffer *buffer; 
    Entries *newDB;
    int                 defStatus;
    Atom                actualType;
    int                 actualFormat;
    unsigned long       nitems, leftover;

    if((data == nullptr) || (*data == '\0'))
    {
	return;
    }
  
   /*
    * Init buffer with input data
    */ 
    buffer = _DtAllocBuffer(&data); 
    if (buffer == nullptr)
	return;

   /*
    * Init, then populate, newDB from buffer
    */
    newDB = _DtAllocEntries();
    if (newDB == nullptr || _DtGetEntries(newDB, buffer, 1) != 0)
    {
	_DtFreeBuffer(buffer);
	_DtFreeEntries(newDB);
	return;
    }

    if (flags & _DT_ATR_RESMGR)
    {
     /*
      * Merge newDB into RESOURCE_MANAGER
      */
      _DtAddToResProp(dpy, _DT_ATR_RESMGR, *newDB);
    }

    if (flags & _DT_ATR_PREFS)
    {
     /*
      * Merge newDB into _DT_SM_PREFERENCES
      */
      _DtAddToResProp(dpy, _DT_ATR_PREFS, *newDB);
    }

   /*
    * Free objects
    */
    _DtFreeBuffer(buffer);
    _DtFreeEntries(newDB);
}
