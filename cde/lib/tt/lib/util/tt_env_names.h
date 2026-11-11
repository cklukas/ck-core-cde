/*
 * tt_env_names.h
 *
 * The environment variables that a message from another program must not set.
 *
 * ttsession takes variables for the program it starts from the contexts of a
 * message, and a program that asks for Set_Environment (see tttk.h) takes them
 * from the request.  The sender of either is not the person who installed the
 * program or runs it, so the names that change which code the program runs
 * or loads, how the shell and the interpreters behave, whom it believes it is,
 * where it finds its credentials, resource files and actions, and the ones
 * ToolTalk itself uses to tie a program to its session, are not passed on.
 * The list cannot be complete; it is one table for every user of it.
 */

#if !defined(_TT_ENV_NAMES_H)
#define _TT_ENV_NAMES_H

/*
 * Returns 1 when name is on the list of names that a message must not set,
 * 0 otherwise.
 */
int		_tt_env_name_unsafe(const char *name);

#endif
