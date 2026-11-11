/*
 * tt_env_names.C
 *
 * The environment variables that a message from another program must not set
 * (see tt_env_names.h).
 */
#include <string.h>
#include "util/tt_env_names.h"

static const char *const unsafe_names[] = {
	// search paths and the behavior of the shell that runs the start string
	"PATH", "IFS", "ENV", "BASH_ENV", "SHELL", "SHELLOPTS", "BASHOPTS",
	"PS4", "CDPATH", "PROMPT_COMMAND",
	// the C library and the resolver
	"GCONV_PATH", "LOCPATH", "NLSPATH", "GLIBC_TUNABLES", "HOSTALIASES",
	"RES_OPTIONS", "RESOLV_HOST_CONF", "LOCALDOMAIN",
	// whom the program believes it is and where it finds its credentials
	"HOME", "USER", "LOGNAME", "XAUTHORITY", "ICEAUTHORITY",
	// where the toolkit and the desktop look for resources and actions
	"XENVIRONMENT", "XFILESEARCHPATH", "XUSERFILESEARCHPATH",
	"XAPPLRESDIR", "XBMLANGPATH", "XLOCALEDIR", "DTDATABASESEARCHPATH",
	// other runtimes
	"CLASSPATH", "JAVA_TOOL_OPTIONS", "_JAVA_OPTIONS", "JDK_JAVA_OPTIONS",
	"NODE_OPTIONS", "NODE_PATH", "TCLLIBPATH",
	0
};

static const char *const unsafe_prefixes[] = {
	"LD_", "DYLD_", "MALLOC_", "BASH_FUNC_",
	"PYTHON", "PERL", "RUBY",
	// ToolTalk
	"TT_", "_TT_", "_SUN_TT_", "TOOLTALK", "TTAUTHORITY", "TTPATH", "TTSESSION",
	0
};

int
_tt_env_name_unsafe(const char *name)
{
	for (const char *const *n = unsafe_names; *n != 0; n++) {
		if (strcmp(*n, name) == 0) {
			return 1;
		}
	}
	for (const char *const *p = unsafe_prefixes; *p != 0; p++) {
		if (strncmp(*p, name, strlen(*p)) == 0) {
			return 1;
		}
	}
	return 0;
}
