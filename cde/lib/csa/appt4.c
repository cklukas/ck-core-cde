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
/* $XConsortium: appt4.c /main/1 1996/04/21 19:21:34 drk $ */
/*
 *  (c) Copyright 1993, 1994 Hewlett-Packard Company
 *  (c) Copyright 1993, 1994 International Business Machines Corp.
 *  (c) Copyright 1993, 1994 Novell, Inc.
 *  (c) Copyright 1993, 1994 Sun Microsystems, Inc.
 */

/*
 * this file contains allocate and free routines for v4 data structures
 */

#include <EUSCompat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "appt4.h"

#define	FAUX_STRING	"Appointment"

/*
 * forward declaration of static functions used within the file
 */
static void free_tag4(Tag_4 *t);
static Attribute_4 *copy_attr4(Attribute_4 *a4);
static Tag_4 *copy_tag4(Tag_4 *t4);

/*****************************************************************************
 * extern functions
 *****************************************************************************/

extern Appt_4 *
_DtCm_make_appt4(boolean_t alloc)
{
	Appt_4 *a;
	
	if ((a = (Appt_4 *)calloc(1, sizeof(Appt_4))) == nullptr)
		return nullptr;

	if ((a->tag = (Tag_4 *)calloc(1, sizeof(Tag_4))) == nullptr) {
		free(a);
		return nullptr;
	}
	a->tag->tag = appointment_4;
	a->tag->next = nullptr;
	a->tag->showtime = B_TRUE;

	a->period.period = single_4;
	a->appt_status = active_4;
	a->privacy = public_4;

	if (alloc) {
		a->what = calloc(1, sizeof(1));
		a->author = calloc(1, sizeof(1));
		a->client_data = calloc(1, sizeof(1));
	}

	a->next = nullptr;
	return(a);
}

extern Appt_4 *
_DtCm_copy_one_appt4(Appt_4 *a4)
{
	Appt_4 *ptr, *copy;

	if (a4 == nullptr)
		return nullptr;

	ptr = a4->next;
	a4->next = nullptr;

	copy = _DtCm_copy_appt4(a4);

	a4->next = ptr;
	return (copy);
}

extern Appt_4 *
_DtCm_copy_appt4(Appt_4 *a4)
{
	Appt_4		*a, *head, *prev;
	boolean_t	cleanup = B_FALSE;

	prev = head = nullptr;
	while (a4 != nullptr) {
		if ((a = (Appt_4 *)calloc(1, sizeof(Appt_4))) == nullptr) {
			cleanup = B_TRUE;
			break;
		}

		a->appt_id.tick = a4->appt_id.tick;
		a->appt_id.key = a4->appt_id.key;

		a->duration = a4->duration;
		a->ntimes = a4->ntimes;

        	if (a4->what) {
			if ((a->what = strdup(a4->what)) == nullptr) {
				_DtCm_free_appt4(a);
				cleanup = B_TRUE;
				break;
			}
		} else if ((a->what = (char *)calloc(1, 1)) == nullptr) {
			_DtCm_free_appt4(a);
			cleanup = B_TRUE;
			break;
		}

		a->period.period = a4->period.period;
		a->period.nth = a4->period.nth;
		a->period.enddate = a4->period.enddate;

        	if (a4->author) {
			if ((a->author = strdup(a4->author)) == nullptr) {
				_DtCm_free_appt4(a);
				cleanup = B_TRUE;
				break;
			}
		} else if ((a->author = (char *)calloc(1, 1)) == nullptr) {
			_DtCm_free_appt4(a);
			cleanup = B_TRUE;
			break;
		}

        	if (a4->client_data) {
			if ((a->client_data = strdup(a4->client_data)) == nullptr) {
				_DtCm_free_appt4(a);
				cleanup = B_TRUE;
				break;
			}
		} else if ((a->client_data = (char *)calloc(1, 1)) == nullptr) {
			_DtCm_free_appt4(a);
			cleanup = B_TRUE;
			break;
		}

		if (a4->exception &&
		    (a->exception = _DtCm_copy_excpt4(a4->exception)) == nullptr) {
			_DtCm_free_appt4(a);
			cleanup = B_TRUE;
			break;
		}

		if (a4->attr && (a->attr = copy_attr4(a4->attr)) == nullptr) {
			_DtCm_free_appt4(a);
			cleanup = B_TRUE;
			break;
		}

		if (a4->tag) {
			if ((a->tag = copy_tag4(a4->tag)) == nullptr) {
				free(a);
				cleanup = B_TRUE;
				break;
			}
		} else if ((a->tag = (Tag_4 *)calloc(1, sizeof(Tag_4))) == nullptr) {
			free(a);
			cleanup = B_TRUE;
			break;
		} else {
			a->tag->tag = appointment_4;
			a->tag->showtime = B_TRUE;
			a->tag->next = nullptr;
		}

		a->appt_status = a4->appt_status;
		a->privacy = a4->privacy;
          	a->next = nullptr;

		if (head == nullptr)
			head = a;
		else
			prev->next = a;
		prev = a;

		a4 = a4->next;
        }

	if (cleanup == B_TRUE) {
		_DtCm_free_appt4(head);
		head = nullptr;
	}

        return (head);
}

/*
 * Copy one appointment.
 * The what field is set to "Appointment".
 */
extern Appt_4 *
_DtCm_copy_semiprivate_appt4(Appt_4 *original)
{
	Appt_4 *temp, *copy = nullptr;

	if (original != nullptr) {
		temp = original->next;
		original->next = nullptr;

		if ((copy = _DtCm_copy_appt4(original)) != nullptr) {
			if (copy->what != nullptr)
				free(copy->what);
			if ((copy->what = (char *)strdup(FAUX_STRING)) == nullptr)
			{
				_DtCm_free_appt4(copy);
				copy = nullptr;
			}
		}

		original->next = temp;
	}
	return(copy);
}

extern Abb_Appt_4 *
_DtCm_appt_to_abbrev4(Appt_4 *original)
{
	Abb_Appt_4 *new = nullptr;

	if (original != nullptr) {
		if ((new = (Abb_Appt_4 *)calloc(1, sizeof(Abb_Appt_4))) == nullptr)
			return nullptr;

		if ((new->tag = copy_tag4(original->tag)) == nullptr) {
			free(new);
			return nullptr;
		}

		new->appt_id.tick = original->appt_id.tick;
		new->appt_id.key = original->appt_id.key;

		if ((new->what = (char *)strdup(original->what)) == nullptr) {
			_DtCm_free_abbrev_appt4(new);
			return nullptr;
		}

		new->duration = original->duration;
		new->period.period = original->period.period;
		new->period.nth = original->period.nth;
		new->period.enddate = original->period.enddate;

		new->appt_status = original->appt_status;
		new->privacy = original->privacy;
		new->next=nullptr;
	}
	return(new);
}

/*
 * The what field is set to "Appointment".
 */
extern Abb_Appt_4 *
_DtCm_appt_to_semiprivate_abbrev4(Appt_4 *original)
{
	Abb_Appt_4 *new = _DtCm_appt_to_abbrev4(original);
	if (new != nullptr) {
		if (new->what != nullptr)
			free(new->what);
		if ((new->what = strdup(FAUX_STRING)) == nullptr) {
			_DtCm_free_abbrev_appt4(new);
			new = nullptr;
		}
	}
	return(new);
}

extern void
_DtCm_free_appt4(Appt_4 *a)
{
	Appt_4 *ptr;

	while (a != nullptr) {
		ptr = a->next;

		if (a->what != nullptr)
			free(a->what);

		if (a->author != nullptr)
			free(a->author);

		if (a->tag != nullptr)
			free_tag4(a->tag);

		if (a->attr != nullptr)
			_DtCm_free_attr4(a->attr);

		if (a->exception != nullptr)
			_DtCm_free_excpt4(a->exception);

     		free(a);

		a = ptr;
	}
}

extern void
_DtCm_free_abbrev_appt4(Abb_Appt_4 *a)
{
	Abb_Appt_4 *ptr;

	while (a != nullptr) {
		ptr = a->next;

		if (a->what != nullptr) {
			free(a->what);
		}

		if (a->tag != nullptr) {
			free_tag4(a->tag);
		}

		free(a);

		a = ptr;
	}
}

Attribute_4 *
_DtCm_make_attr4(void)
{
	Attribute_4 *a;
	
	if ((a = (Attribute_4 *)calloc(1, sizeof(Attribute_4))) == nullptr)
		return nullptr;

	if ((a->attr = (char *)calloc(1, 1)) == nullptr) {
		free(a);
		return nullptr;
	}

	if ((a->value = (char *)calloc(1, 1)) == nullptr) {
		_DtCm_free_attr4(a);
		return nullptr;
	}

	if ((a->clientdata = (char *)calloc(1, 1)) == nullptr) {
		_DtCm_free_attr4(a);
		return nullptr;
	}

	return(a);
}

extern void
_DtCm_free_attr4(Attribute_4 *a)
{
	Attribute_4 *ptr;

	while (a != nullptr) {
		ptr = a->next;
		if (a->attr != nullptr)
			free(a->attr);
		if (a->value != nullptr)
			free(a->value);
		if (a->clientdata != nullptr)
			free(a->clientdata);
		free(a);

		a = ptr;
	}
}

extern Reminder_4 *
_DtCm_copy_reminder4(Reminder_4 *r4)
{
        Reminder_4 *rem, *head, *prev;

	prev = head = nullptr;
	while (r4 != nullptr) {
		rem = (Reminder_4 *)calloc(1, sizeof(Reminder_4));
		rem->appt_id = r4->appt_id;
		rem->tick = r4->tick;
		rem->attr.next = nullptr;
		rem->attr.attr = strdup(r4->attr.attr);
		rem->attr.value = strdup(r4->attr.value);
		rem->attr.clientdata = strdup(r4->attr.clientdata);
		rem->next = nullptr;

		if (head == nullptr)
			head = rem;
		else
			prev->next = rem;
		prev = rem;

		r4 = r4->next;
	}
	return(head);
}
      
extern void
_DtCm_free_reminder4(Reminder_4 *r)
{
	Reminder_4 *ptr;

	while (r != nullptr) {
		ptr = r->next;

		if (r->attr.attr != nullptr)
			free (r->attr.attr);
		if (r->attr.value != nullptr)
			free (r->attr.value);
		if (r->attr.clientdata != nullptr)
			free (r->attr.clientdata);
		free (r);

		r = ptr;
	}
}

extern void
_DtCm_free_keyentry4(Uid_4 *k)
{
	Uid_4 *ptr;

	while (k != nullptr) {
		ptr = k->next;
		free(k);
		k = ptr;
	}
}

extern Access_Entry_4 *
_DtCm_make_access_entry4(char *who, int perms)
{
        Access_Entry_4 *e;

        if (who==nullptr) return((Access_Entry_4 *)nullptr);
        if ((e = (Access_Entry_4 *)calloc(1, sizeof(Access_Entry_4))) == nullptr)
		return nullptr;

        if ((e->who = strdup(who)) == nullptr) {
		free(e);
		return nullptr;
	}

        e->access_type = perms;
        e->next = nullptr;
        return(e);
}

extern void
_DtCm_free_access_list4(Access_Entry_4 *e)
{
	Access_Entry_4 *ptr;

	while (e != nullptr) {
		ptr = e->next;

		if (e->who != nullptr)
			free(e->who);
		free(e);

		e = ptr;
	}
}

extern void
_DtCm_free_excpt4(Except_4 *e)
{
	Except_4 *ptr;

	while (e != nullptr) {
		ptr = e->next;
		free(e);
		e = ptr;
	}
}

extern Except_4 *
_DtCm_copy_excpt4(Except_4 *e4)
{
	Except_4	*e, *head, *prev;
	boolean_t	cleanup = B_FALSE;

	prev = head = nullptr;
	while (e4 != nullptr) {
		if ((e = (Except_4 *)calloc(1, sizeof(Except_4))) == nullptr) {
			cleanup = B_TRUE;
			break;
		}

		e->ordinal = e4->ordinal;
		e->next = nullptr;

		if (head == nullptr)
			head = e;
		else
			prev->next = e;
		prev = e;

		e4 = e4->next;
	}

	if (cleanup == B_TRUE) {
		_DtCm_free_excpt4(head);
		head = nullptr;
	}

	return (head);
}

/******************************************************************************
 * static functions used within the file
 ******************************************************************************/

static void
free_tag4(Tag_4 *t)
{
	Tag_4 *ptr;

	while (t != nullptr) {
		ptr = t->next;
		free(t);
		t = ptr;
	}
}

/*	Copy an attribute list recursively */
static Attribute_4 *
copy_attr4(Attribute_4 *a4)
{
	Attribute_4	*a, *head, *prev;
	boolean_t	cleanup = B_FALSE;

	prev = head = nullptr;
	while (a4 != nullptr) {
		if ((a = (Attribute_4 *)calloc(1, sizeof(Attribute_4)))
		    == nullptr) {
			cleanup = B_TRUE;
			break;
		}

		if ((a->attr = strdup(a4->attr)) == nullptr) {
			free(a);
			cleanup = B_TRUE;
			break;
		}
	
		if ((a->value = strdup(a4->value)) == nullptr) {
			_DtCm_free_attr4(a);
			cleanup = B_TRUE;
			break;
		}

		if ((a->clientdata = strdup(a4->clientdata)) == nullptr) {
			_DtCm_free_attr4(a);
			cleanup = B_TRUE;
			break;
		}

		if (head == nullptr)
			head = a;
		else
			prev->next = a;
		prev = a;

		a4 = a4->next;
	}

	if (cleanup == B_TRUE) {
		_DtCm_free_attr4(head);
		head = nullptr;
	}

	return (head);
}

static Tag_4 *
copy_tag4(Tag_4 *t4)
{
	Tag_4		*t, *head, *prev;
	boolean_t	cleanup = B_FALSE;

	prev = head = nullptr;
	while (t4 != nullptr) {
		if ((t = (Tag_4 *)calloc(1, sizeof(Tag_4))) == nullptr) {
			cleanup = B_TRUE;
			break;
		}

		t->tag = t4->tag;
		t->showtime = t4->showtime;
		t->next = nullptr;

		if (head == nullptr)
			head = t;
		else
			prev->next = t;
		prev = t;

		t4 = t4->next;
	}

	if (cleanup == B_TRUE) {
		free_tag4(head);
		head = nullptr;
	}

	return (head);
}

