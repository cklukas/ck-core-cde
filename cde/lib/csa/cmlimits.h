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

#ifndef _CMLIMITS_H
#define _CMLIMITS_H

/*
 * Limits of the calendar RPC protocols (versions 2 to 5 and the callback
 * protocol).
 *
 * Every string, byte string, array and list that the XDR routines of libcsa
 * code is bounded by one of these.  The XDR routines read the length of a
 * string or the number of elements from the message and allocate that much
 * before they read the contents, so a limit is what makes the decoding of a
 * message from the network safe.  The same limits apply to what is sent: a
 * request or a reply that exceeds one cannot be encoded and fails instead
 * of being cut.
 */

/* names of calendars and users: user@host; a user name has at most 256
 * characters and a host name at most 255 */
#define _DtCM_LIMIT_NAME		512

/* names of attributes: the defined ones are shorter than 64 characters */
#define _DtCM_LIMIT_ATTR_NAME		256

/* names of character sets and identifiers of entries */
#define _DtCM_LIMIT_CHARSET		64
#define _DtCM_LIMIT_ENTRY_ID		256

/* date and time, date and time ranges and durations in ISO 8601 notation */
#define _DtCM_LIMIT_DATE_TIME		128

/* text: a string attribute, the text of an appointment, a reminder lead time
 * or the data of an opaque attribute value */
#define _DtCM_LIMIT_TEXT		65536

/* the text of an archived calendar, sent as a whole */
#define _DtCM_LIMIT_ARCHIVE		(64 * 1024 * 1024)

/* attributes, and names of attributes, of a calendar, an entry or a request,
 * and the reminders of an appointment */
#define _DtCM_LIMIT_ATTRIBUTES		1024

/* entries, appointments, reminders and keys in a request or reply */
#define _DtCM_LIMIT_ENTRIES		(1 << 20)
#define _DtCM_LIMIT_KEYS		(1 << 16)

/* calendar names in the list of calendars of a host */
#define _DtCM_LIMIT_CALENDARS		(1 << 16)

/* entries of an access list and of a list of dates and times, and the other
 * items of a list in a request: instances, ranges, exceptions */
#define _DtCM_LIMIT_ACCESS_ENTRIES	(1 << 16)
#define _DtCM_LIMIT_ITEMS		(1 << 16)

/* the event types of an appointment: an appointment has one */
#define _DtCM_LIMIT_TAGS		16

/* the registrations of a request: one per request */
#define _DtCM_LIMIT_REGISTRATIONS	16

#endif /* _CMLIMITS_H */
