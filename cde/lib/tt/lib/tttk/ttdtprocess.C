/*
 * ttdtprocess.C
 *
 * What a Desktop message may change in the process that receives it, and the
 * checks that go with it (see ttdtprocess.h).
 */
#include "tt_options.h"
#include <errno.h>
#include <limits.h>
#include <locale.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "util/tt_env_names.h"
#include "util/tt_port.h"
#include "tttk/ttdtprocess.h"

extern Tt_status	_tt_errno_status( int err_no );

//
// Letters and digits of ASCII, whatever the locale of the process is: the
// locale is one of the things these messages change
//
static int
_ttDtAlnum(unsigned char c)
{
	return ((c >= '0') && (c <= '9')) || ((c >= 'A') && (c <= 'Z')) ||
	       ((c >= 'a') && (c <= 'z'));
}

Tt_status
_ttdt_arg_string(
	Tt_message	msg,
	int		n,
	size_t		maxLen,
	char	      **value
)
{
	*value = 0;
	unsigned char *bytes;
	int len;
	Tt_status status = tt_message_arg_bval( msg, n, &bytes, &len );
	if (status != TT_OK) {
		return status;
	}
	if ((bytes == 0) || (len < 0)) {
		return TT_DESKTOP_EPROTO;
	}
	if ((size_t)len > maxLen) {
		return TT_DESKTOP_EINVAL;
	}
	if (memchr( bytes, '\0', len ) != 0) {
		return TT_DESKTOP_EPROTO;
	}
	char *copy = (char *)tt_malloc( len + 1 );
	if (copy == 0) {
		return TT_ERR_NOMEM;
	}
	memcpy( copy, bytes, len );
	copy[ len ] = '\0';
	*value = copy;
	return TT_OK;
}

int
_ttdt_category(const char *name)
{
	if (name == 0) {
		return -1;
	}
	static const struct {
		const char *name;
		int category;
	} categories[] = {
		{ "LC_CTYPE",    LC_CTYPE },
		{ "LC_NUMERIC",  LC_NUMERIC },
		{ "LC_TIME",     LC_TIME },
		{ "LC_COLLATE",  LC_COLLATE },
		{ "LC_MONETARY", LC_MONETARY },
		{ "LC_MESSAGES", LC_MESSAGES },
		{ "LC_ALL",      LC_ALL }
	};
	for (unsigned int i = 0; i < sizeof( categories ) / sizeof( categories[0] ); i++) {
		if (strcmp( name, categories[ i ].name ) == 0) {
			return categories[ i ].category;
		}
	}
	return -1;
}

int
_ttdt_locale_valid(const char *name)
{
	if (name == 0) {
		return 0;
	}
	size_t len = strlen( name );
	if ((len == 0) || (len > _TTDT_MAX_LOCALE_NAME)) {
		return 0;
	}
	if (! _ttDtAlnum( (unsigned char)name[ 0 ] )) {
		return 0;
	}
	for (size_t i = 0; i < len; i++) {
		unsigned char c = name[ i ];
		if (! _ttDtAlnum( c ) && (strchr( "_.@+,=;-", c ) == 0)) {
			return 0;
		}
	}
	return strstr( name, ".." ) == 0;
}

int
_ttdt_directory_valid(const char *path)
{
	if ((path == 0) || (path[ 0 ] != '/')) {
		return 0;
	}
	size_t len = strlen( path );
	if (len >= PATH_MAX) {
		return 0;
	}
	for (size_t i = 0; i < len; i++) {
		unsigned char c = path[ i ];
		if ((c < 0x20) || (c == 0x7f)) {
			return 0;
		}
	}
	return 1;
}

int
_ttdt_display_valid(const char *display)
{
	if (display == 0) {
		return 0;
	}
	size_t len = strlen( display );
	if ((len == 0) || (len > _TTDT_MAX_DISPLAY)) {
		return 0;
	}
	if (! _ttDtAlnum( (unsigned char)display[ 0 ] ) &&
	    (strchr( ":/[", display[ 0 ] ) == 0)) {
		return 0;
	}
	if (strchr( display, ':' ) == 0) {
		return 0;
	}
	for (size_t i = 0; i < len; i++) {
		unsigned char c = display[ i ];
		if (! _ttDtAlnum( c ) && (strchr( "._-:/[]%+@,", c ) == 0)) {
			return 0;
		}
	}
	return 1;
}

int
_ttdt_variable_name_valid(const char *name)
{
	if (name == 0) {
		return 0;
	}
	size_t len = strlen( name );
	if ((len == 0) || (len > _TTDT_MAX_VARIABLE_NAME)) {
		return 0;
	}
	if ((name[ 0 ] >= '0') && (name[ 0 ] <= '9')) {
		return 0;
	}
	for (size_t i = 0; i < len; i++) {
		unsigned char c = name[ i ];
		if (! _ttDtAlnum( c ) && (c != '_')) {
			return 0;
		}
	}
	return 1;
}

Tt_status
_ttdt_adopt_locale(
	const char     *const *categories,
	const char     *const *locales
)
{
	int i;
	for (i = 0; (categories[ i ] != 0) && (i <= _TTDT_MAX_LOCALE_PAIRS); i++) {
		if (_ttdt_category( categories[ i ] ) == -1) {
			return TT_DESKTOP_EINVAL;
		}
		if ((locales[ i ] != 0) && ! _ttdt_locale_valid( locales[ i ] )) {
			return TT_DESKTOP_EINVAL;
		}
	}
	if (i > _TTDT_MAX_LOCALE_PAIRS) {
		return TT_DESKTOP_EINVAL;
	}
	Tt_status status = TT_OK;
	for (i = 0; categories[ i ] != 0; i++) {
		if (locales[ i ] == 0) {
			continue;
		}
		if (setlocale( _ttdt_category( categories[ i ] ), locales[ i ] ) == 0) {
			// the system does not have it; go on with the others
			status = TT_DESKTOP_EINVAL;
		}
	}
	return status;
}

Tt_status
_ttdt_adopt_directory(const char *path)
{
	if (! _ttdt_directory_valid( path )) {
		return TT_DESKTOP_EINVAL;
	}
	if (chdir( path ) != 0) {
		return _tt_errno_status( errno );
	}
	return TT_OK;
}

Tt_status
_ttdt_adopt_display(const char *display)
{
	if (! _ttdt_display_valid( display )) {
		return TT_DESKTOP_EINVAL;
	}
	if (_tt_putenv( "DISPLAY", display ) == 0) {
		return TT_ERR_NOMEM;
	}
	return TT_OK;
}

Tt_status
_ttdt_adopt_variable(const char *name, const char *value)
{
	if (! _ttdt_variable_name_valid( name )) {
		return TT_DESKTOP_EINVAL;
	}
	if (_tt_env_name_unsafe( name )) {
		return TT_DESKTOP_EACCES;
	}
	if ((value != 0) && (strlen( value ) > _TTDT_MAX_VARIABLE_VALUE)) {
		return TT_DESKTOP_EINVAL;
	}
	if (value == 0) {
		if (unsetenv( name ) != 0) {
			return _tt_errno_status( errno );
		}
		return TT_OK;
	}
	if (_tt_putenv( name, value ) == 0) {
		return TT_ERR_NOMEM;
	}
	return TT_OK;
}
