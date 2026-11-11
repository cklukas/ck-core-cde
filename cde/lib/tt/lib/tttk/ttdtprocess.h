/*
 * ttdtprocess.h
 *
 * What a Desktop message may change in the process that receives it, and the
 * checks that go with it.
 *
 * Two kinds of messages change the process.  A request that the program has
 * asked for (Set_Locale, Set_Situation, Set_Environment, Signal; see
 * ttdt_session_join()) carries the values in its arguments, and the replies
 * that ttdt_sender_imprint_on() has asked for carry the values of the
 * handler.  Either way the sender is another program, so what it sends is
 * bounded, must not have a null byte inside it, and must have the shape of a
 * locale name, a directory, a display or a variable before anything is done
 * with it.  Nothing is changed unless every value that belongs together has
 * passed.
 */
#ifndef ttdtprocess_h
#define ttdtprocess_h

#include <stddef.h>
#include "Tt/tt_c.h"

// How much of each there can be, and how long each can be
#define _TTDT_MAX_LOCALE_PAIRS		16	// categories in a locale message
#define _TTDT_MAX_CATEGORY_NAME		32
#define _TTDT_MAX_VARIABLES		128	// variables in an environment message
#define _TTDT_MAX_LOCALE_NAME		1024	// the composite name of LC_ALL is the longest
#define _TTDT_MAX_DISPLAY		512
#define _TTDT_MAX_VARIABLE_NAME		255
#define _TTDT_MAX_VARIABLE_VALUE	32768

//
// The value of argument n of msg as a string to keep: set, at most maxLen
// bytes, with no null byte inside.  *value is tt_free()d by the caller.
//	TT_DESKTOP_EPROTO	the argument is not set, or has a null byte
//				inside
//	TT_DESKTOP_EINVAL	longer than maxLen
//
Tt_status	_ttdt_arg_string(
			Tt_message	msg,
			int		n,
			size_t		maxLen,
			char	      **value
		);

//
// The category of <locale.h> that a name stands for, -1 if it stands for none
//
int		_ttdt_category(const char *name);

//
// The shapes of the values.  Each returns 1 for a value of that shape.
//	locale		a name for setlocale(): letters, digits and _.@+,=;-,
//			beginning with a letter or digit, without "..", so that
//			it cannot name a path
//	directory	an absolute path, shorter than PATH_MAX, without
//			control characters
//	display		a name for $DISPLAY: host, colon, number, or the path of
//			a socket, without blanks or shell characters
//	variable name	a name of the shell: letters, digits and _, not
//			beginning with a digit
//
int		_ttdt_locale_valid(const char *name);
int		_ttdt_directory_valid(const char *path);
int		_ttdt_display_valid(const char *display);
int		_ttdt_variable_name_valid(const char *name);

//
// Change the process.  Each checks what it is given first and changes nothing
// if it does not pass (TT_DESKTOP_EINVAL; TT_DESKTOP_EACCES for a variable
// that no message may set, see util/tt_env_names.h).
//
// categories and locales are arrays of the same length, ended by a null
// category; a null locale is a category the sender did not answer for.
//
Tt_status	_ttdt_adopt_locale(
			const char     *const *categories,
			const char     *const *locales
		);
Tt_status	_ttdt_adopt_directory(const char *path);
Tt_status	_ttdt_adopt_display(const char *display);
// value 0 removes the variable
Tt_status	_ttdt_adopt_variable(const char *name, const char *value);

#endif
