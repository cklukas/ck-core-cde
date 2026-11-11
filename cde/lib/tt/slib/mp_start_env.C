/*
 * Tool Talk Message Passer (MP) - mp_start_env.C
 *
 * The policy for the environment variables a message may set in the process
 * ttsession starts for it (see mp_start_env.h).
 */
#include <ctype.h>
#include <string.h>
#include "mp_start_env.h"
#include "util/tt_env_names.h"

// The names a strict policy passes on its own.
static const char *const strict_names[] = {
	"DISPLAY", "LANG", "LANGUAGE", "TERM", "TZ",
	0
};

static const char *const strict_prefixes[] = {
	"LC_",
	0
};

static int
in_names(const char *const *names, const char *name)
{
	for (; *names != 0; names++) {
		if (strcmp(*names, name) == 0) {
			return 1;
		}
	}
	return 0;
}

static int
has_prefix(const char *const *prefixes, const char *name)
{
	for (; *prefixes != 0; prefixes++) {
		if (strncmp(*prefixes, name, strlen(*prefixes)) == 0) {
			return 1;
		}
	}
	return 0;
}

// The entries of a setting: names, "PREFIX*" and "strict", between commas.
// Spaces around an entry are ignored.
static const char *
next_entry(const char *list, const char **begin, size_t *length)
{
	while (*list == ',' || isspace((unsigned char)*list)) {
		list++;
	}
	if (*list == '\0') {
		return 0;
	}
	*begin = list;
	while (*list != '\0' && *list != ',' &&
	       !isspace((unsigned char)*list)) {
		list++;
	}
	*length = list - *begin;
	return list;
}

// Whether an entry is a name of the shape the slot names of contexts have,
// with an optional "*" at the end.
static int
well_formed(const char *entry, size_t length)
{
	if (length > 0 && entry[length - 1] == '*') {
		length--;
	}
	if (length == 0) {
		return 0;
	}
	for (size_t i = 0; i < length; i++) {
		if (!isalnum((unsigned char)entry[i]) && entry[i] != '_') {
			return 0;
		}
	}
	return 1;
}

static int
is_strict(const char *entry, size_t length)
{
	return length == 6 && strncmp(entry, "strict", 6) == 0;
}

_Tt_start_env::
_Tt_start_env()
{
	_strict = 0;
}

int _Tt_start_env::
configure(const char *setting)
{
	const char	*list = setting;
	const char	*entry;
	size_t		length;
	int		bad = 0;

	_strict = 0;
	_setting = "";
	if (setting == 0) {
		return 0;
	}
	_setting = setting;
	while ((list = next_entry(list, &entry, &length)) != 0) {
		if (is_strict(entry, length)) {
			_strict = 1;
		} else if (!well_formed(entry, length)) {
			bad++;
		}
	}
	if (bad) {
		// a setting that is not understood is read as the strictest
		// reading that is still usable
		_strict = 1;
	}
	return bad;
}

int _Tt_start_env::
approved(const char *name) const
{
	const char	*list = _setting;
	const char	*entry;
	size_t		length;
	size_t		name_length = strlen(name);

	if (list == 0) {
		return 0;
	}
	while ((list = next_entry(list, &entry, &length)) != 0) {
		if (is_strict(entry, length) || !well_formed(entry, length)) {
			continue;
		}
		if (entry[length - 1] == '*') {
			if (strncmp(entry, name, length - 1) == 0) {
				return 1;
			}
		} else if (length == name_length &&
			   strncmp(entry, name, length) == 0) {
			return 1;
		}
	}
	return 0;
}

_Tt_start_env_verdict _Tt_start_env::
check(const char *name) const
{
	if (approved(name)) {
		return _TT_START_ENV_OK;
	}
	if (_strict) {
		if (in_names(strict_names, name) ||
		    has_prefix(strict_prefixes, name)) {
			return _TT_START_ENV_OK;
		}
		return _TT_START_ENV_NOT_APPROVED;
	}
	if (_tt_env_name_unsafe(name)) {
		return _TT_START_ENV_UNSAFE;
	}
	return _TT_START_ENV_OK;
}
