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
/* $XConsortium: rerule.c /main/4 1995/11/09 12:50:23 rswiston $ */
/*
 *  (c) Copyright 1993, 1994 Hewlett-Packard Company
 *  (c) Copyright 1993, 1994 International Business Machines Corp.
 *  (c) Copyright 1993, 1994 Novell, Inc.
 *  (c) Copyright 1993, 1994 Sun Microsystems, Inc.
 */

/*
 * Text form of a parsed recurrence rule.  The text is built in a buffer
 * that grows with the rule, so a rule with any number of times, days or
 * weeks has a complete text of its own.
 */

#include <stdarg.h>
#include <stdio.h>
#include <ctype.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include "rerule.h"

typedef enum {
        NUM_NUM         = 0,    /* Used in NumsToBuf to tell the proc what  */
        NUM_DAY,                /* type of list we are handing it.          */
        NUM_WEEK
} NumType;

/* The text of a rule so far; failed is set once memory has run out. */
typedef struct {
	char	*data;
	size_t	 length;
	size_t	 size;
	int	 failed;
} ReText;

static void NumsToBuf(unsigned int *, unsigned int, NumType, ReText *);
static void WeekNumberToText(WeekNumber, ReText *);
static void WeekDayToText(WeekDay, ReText *);
static void ConvertDaily(RepeatEvent *, ReText *);
static void ConvertWeekly(RepeatEvent *, ReText *);
static void ConvertMonthly(RepeatEvent *, ReText *);
static void ConvertYearly(RepeatEvent *, ReText *);

#define RE_DAILY(re) (re->re_data.re_daily)
#define RE_WEEKLY(re) (re->re_data.re_weekly)
#define RE_MONTHLY(re) (re->re_data.re_monthly)
#define RE_YEARLY(re) (re->re_data.re_yearly)

/*
 * Append formatted text, growing the buffer to the length of the result.
 */
static void
ReTextAppend(
	ReText		*text,
	const char	*format,
	...)
{
	va_list	 args;
	va_list	 again;
	int	 needed;
	char	*grown;

	if (text->failed)
		return;

	va_start(args, format);
	va_copy(again, args);
	needed = vsnprintf(NULL, 0, format, args);
	va_end(args);

	if (needed < 0) {
		text->failed = 1;
		va_end(again);
		return;
	}

	if (text->length + (size_t)needed + 1 > text->size) {
		size_t size = text->size ? text->size : 64;

		while (size < text->length + (size_t)needed + 1)
			size *= 2;
		if ((grown = realloc(text->data, size)) == NULL) {
			text->failed = 1;
			va_end(again);
			return;
		}
		text->data = grown;
		text->size = size;
	}

	vsnprintf(text->data + text->length, text->size - text->length,
		  format, again);
	va_end(again);
	text->length += (size_t)needed;
}

/*
 * Returns the text of the rules of the list, one after the other and
 * separated by a blank, in memory the caller frees; NULL for no rule or
 * when memory runs out.
 */
char *
ReToString(
	RepeatEvent *re)
{
	ReText	 text = { NULL, 0, 0, 0 };

	if (!re) return (char *)NULL;

	while (re) {
		size_t	start = text.length;

		if (start > 0)
			ReTextAppend(&text, " ");

		switch (re->re_type) {

		case RT_MINUTE:
			ReTextAppend(&text, "M%d #%d",
					re->re_interval, re->re_duration);
			break;
		case RT_DAILY:
			ConvertDaily(re, &text);
			break;
		case RT_WEEKLY:
			ConvertWeekly(re, &text);
			break;
		case RT_MONTHLY_POSITION:
		case RT_MONTHLY_DAY:
			ConvertMonthly(re, &text);
			break;
		case RT_YEARLY_MONTH:
		case RT_YEARLY_DAY:
			ConvertYearly(re, &text);
			break;
		default:
			/* nothing to say about a rule of an unknown type */
			if (text.data != NULL)
				text.data[text.length = start] = '\0';
			break;
		}

		re = re->re_next;
	}

	if (text.failed) {
		free(text.data);
		return (char *)NULL;
	}

	/* rules of unknown types have no text; that is the empty text */
	if (text.data == NULL)
		return strdup("");

	return text.data;
}

/*
 * Takes an array of numbers, converts them back into their string
 * type (e.g. SU 1W etc) and appends them to the text with end
 * marks as necessary, separated by spaces.
 */
static void
NumsToBuf(
	unsigned int	*array,
	unsigned int	 array_size,
	NumType		 type,
	ReText		*text)
{
	unsigned int	i;

	for (i = 0; i < array_size && !text->failed; i++) {
		if (type == NUM_NUM)
			ReTextAppend(text, " %d", RE_MASK_STOP(array[i]));
		else if (type == NUM_DAY)
			WeekDayToText(RE_MASK_STOP(array[i]), text);
		else if (type == NUM_WEEK)
			WeekNumberToText(RE_MASK_STOP(array[i]), text);

			/* Add end mark if needed */
		if (RE_STOP_IS_SET(array[i]))
			ReTextAppend(text, "$");
	}
}

static void
WeekDayToText(
	WeekDay		 day,
	ReText		*text)
{
	switch (RE_MASK_STOP(day)) {
	case WD_SUN:
		ReTextAppend(text, " SU");
		break;
	case WD_MON:
		ReTextAppend(text, " MO");
		break;
	case WD_TUE:
		ReTextAppend(text, " TU");
		break;
	case WD_WED:
		ReTextAppend(text, " WE");
		break;
	case WD_THU:
		ReTextAppend(text, " TH");
		break;
	case WD_FRI:
		ReTextAppend(text, " FR");
		break;
	case WD_SAT:
		ReTextAppend(text, " SA");
		break;
	}

	if (RE_STOP_IS_SET(day))
		ReTextAppend(text, "$");
}

static void
WeekNumberToText(
	WeekNumber	 week,
	ReText		*text)
{
	switch (RE_MASK_STOP(week)) {
	case WK_F1:
		ReTextAppend(text, " 1+");
		break;
	case WK_F2:
		ReTextAppend(text, " 2+");
		break;
	case WK_F3:
		ReTextAppend(text, " 3+");
		break;
	case WK_F4:
		ReTextAppend(text, " 4+");
		break;
	case WK_F5:
		ReTextAppend(text, " 5+");
		break;
	case WK_L1:
		ReTextAppend(text, " 1-");
		break;
	case WK_L2:
		ReTextAppend(text, " 2-");
		break;
	case WK_L3:
		ReTextAppend(text, " 3-");
		break;
	case WK_L4:
		ReTextAppend(text, " 4-");
		break;
	case WK_L5:
		ReTextAppend(text, " 5-");
		break;
	}

	if (RE_STOP_IS_SET(week))
		ReTextAppend(text, "$");
}

static void
ConvertDaily(
	RepeatEvent	*re,
	ReText		*text)
{
	ReTextAppend(text, "D%d", re->re_interval);

	NumsToBuf((unsigned int *)RE_DAILY(re)->dd_time,
		  RE_DAILY(re)->dd_ntime, NUM_NUM, text);

		/* Tack on the duration information */
	ReTextAppend(text, " #%d", re->re_duration);
}

static void
ConvertWeekly(
	RepeatEvent	*re,
	ReText		*text)
{
	unsigned int	i;

	ReTextAppend(text, "W%d", re->re_interval);

		/* walk through Day/time data (e.g. TU 1200 Th 2000)	*/
	for (i = 0; i < RE_WEEKLY(re)->wd_ndaytime; i++) {

			/* The day: MO TU TH etc. */
		WeekDayToText(RE_WEEKLY(re)->wd_daytime[i].dt_day, text);

			/* The hours: 1000 1400 etc. */
		NumsToBuf((unsigned int *)RE_WEEKLY(re)->wd_daytime[i].dt_time,
			  RE_WEEKLY(re)->wd_daytime[i].dt_ntime, NUM_NUM, text);
	}
		/* Tack on the duration information */
	ReTextAppend(text, " #%d", re->re_duration);
}

static void
ConvertMonthly(
	RepeatEvent	*re,
	ReText		*text)
{
	unsigned int	i;
	unsigned int	num_items = RE_MONTHLY(re)->md_nitems;

	if (re->re_type == RT_MONTHLY_POSITION)
		ReTextAppend(text, "MP%d", re->re_interval);
	else
		ReTextAppend(text, "MD%d", re->re_interval);

	if (re->re_type == RT_MONTHLY_POSITION) {

		/* walk through Day/time data (e.g. TU 1200 Th 2000)	*/
	    for (i = 0; i < num_items; i++) {

			/* The week: 1+ 3- etc. */
		NumsToBuf(
			 (unsigned int*)RE_MONTHLY(re)->md_weektime[i].wdt_week,
			  RE_MONTHLY(re)->md_weektime[i].wdt_nweek, NUM_WEEK,
			  text);

			/* The day: SU MO TU etc. */
		NumsToBuf(
			 (unsigned int *)RE_MONTHLY(re)->md_weektime[i].wdt_day,
			  RE_MONTHLY(re)->md_weektime[i].wdt_nday, NUM_DAY,
			  text);

			/* The hours: 1000 1400 etc. */
		NumsToBuf(
			 (unsigned int *)
				RE_MONTHLY(re)->md_weektime[i].wdt_time,
			  RE_MONTHLY(re)->md_weektime[i].wdt_ntime, NUM_NUM,
			  text);
	    }
	} else {  /* RT_MONTHLY_DAY */
			/* The days: 1 15 31 etc. */
		NumsToBuf((unsigned int *)RE_MONTHLY(re)->md_days,
			  num_items, NUM_NUM, text);
	}
		/* Tack on the duration information */
	ReTextAppend(text, " #%d", re->re_duration);
}

static void
ConvertYearly(
	RepeatEvent	*re,
	ReText		*text)
{
	if (re->re_type == RT_YEARLY_MONTH)
		ReTextAppend(text, "YM%d", re->re_interval);
	else
		ReTextAppend(text, "YD%d", re->re_interval);

		/* An array of days or months */
	NumsToBuf(RE_YEARLY(re)->yd_items,
		  RE_YEARLY(re)->yd_nitems, NUM_NUM, text);

		/* Tack on the duration information */
	ReTextAppend(text, " #%d", re->re_duration);
}
