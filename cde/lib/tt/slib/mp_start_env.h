/*
 * Tool Talk Message Passer (MP) - mp_start_env.h
 *
 * Which environment variables a message may set in the process that
 * ttsession starts for it.
 *
 * A context of a message whose slot name begins with '$' becomes an
 * environment variable of the process ttsession starts for the message
 * (see _Tt_s_message::set_start_env).  The sender of the message is not the
 * one who installed the ptype or runs the session, so a name that changes
 * which code the started program runs or loads, whom it believes it is, or
 * where it finds its credentials, is not passed on.
 *
 * The owner of the session can change this by setting TOOLTALK_START_ENV
 * for ttsession.  Its value is a comma separated list of entries:
 *
 *	strict		pass only DISPLAY, LANG, LANGUAGE, LC_*, TERM and TZ,
 *			and the names the other entries approve
 *	NAME		pass this variable, also if it is one of the unsafe
 *			names
 *	PREFIX*		pass the variables whose names begin with PREFIX
 *
 * An entry that is not one of these makes the policy strict.
 */
#ifndef _MP_START_ENV_H
#define _MP_START_ENV_H

#include "util/tt_string.h"

enum _Tt_start_env_verdict {
	_TT_START_ENV_OK,
	_TT_START_ENV_UNSAFE,		// a name that is never passed by default
	_TT_START_ENV_NOT_APPROVED	// strict policy: not on the list
};

class _Tt_start_env {
      public:
					_Tt_start_env();

	// Applies the setting of the owner (see above).  Returns the
	// number of entries that were not understood; the policy is
	// strict then.
	int				configure(const char *setting);
	_Tt_start_env_verdict		check(const char *name) const;
	int				strict() const {
		return _strict;
	}

      private:
	int				approved(const char *name) const;

	int				_strict;
	_Tt_string			_setting;	// as the owner wrote it
};

#endif /* _MP_START_ENV_H */
