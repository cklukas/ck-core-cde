/*
 * Regression tests for the termios string of the SPC protocol
 * (DtEncap/spc-termio.c) and for the error texts of the SPC library
 * (DtEncap/spc-error.c): the string is a description of the terminal
 * settings that a peer sends, and the error texts carry what the peer sent.
 *
 * check-spc-termio.py compiles the production sources next to this program
 * under AddressSanitizer and UndefinedBehaviorSanitizer and links the built
 * libDtSvc for the rest.  Each case runs in its own process.
 */
#include <bms/sbport.h> /* the first include of the SPC sources */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include <SPC/spcP.h>

#ifdef OLD_ENCODE_RETURNS_VOID
/* the routine returned nothing; every string was taken for a good one */
#define ENCODE(s, t) (SPC_Encode_Termios((s), (t)), 1)
#else
#define ENCODE(s, t) SPC_Encode_Termios((s), (t))
#endif

static struct termios sample(void)
{
	struct termios t;

	memset(&t, 0, sizeof t);
	t.c_iflag = BRKINT | IGNPAR | ICRNL | IXON;
	t.c_oflag = OPOST | ONLCR;
	t.c_cflag = CS7 | CREAD | CLOCAL;
	t.c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK;
	t.c_cc[VERASE] = 101;
	t.c_cc[VEOF] = 102;
	t.c_cc[VSTOP] = 103;
	cfsetispeed(&t, B9600);
	cfsetospeed(&t, B1200);
	return t;
}

static int same(const struct termios *a, const struct termios *b)
{
	return a->c_iflag == b->c_iflag && a->c_oflag == b->c_oflag && a->c_cflag == b->c_cflag &&
	       a->c_lflag == b->c_lflag && memcmp(a->c_cc, b->c_cc, NCCS) == 0 &&
	       cfgetispeed(a) == cfgetispeed(b) && cfgetospeed(a) == cfgetospeed(b);
}

/* The string for a good description, as the library writes it. */
static char *good_string(void)
{
	struct termios t = sample();

	return strdup(SPC_Decode_Termios(&t));
}

static void scenario_round_trip(void)
{
	struct termios t = sample(), back;
	char *s = good_string();

	memset(&back, 0, sizeof back);
	assert(ENCODE(s, &back));
	assert(same(&t, &back));
	free(s);
}

/* A string that is not a whole description changes nothing. */
static void scenario_refusals(void)
{
	char *good = good_string();
	size_t i;
	static const char *const replace[][2] = {
		{ "XPG3", "XPG9" },		/* another version */
		{ "BRKINT", "NOSUCHFLAG" },	/* a mode that does not exist */
		{ "B1200", "B7" },		/* a speed that does not exist */
		{ "VERASE=101", "VERASE=300" },	/* a value that is no control character */
		{ "VEOF=102", "VEOF" },		/* a value that is not there */
		{ "VEOF=102", "VNOSUCH=1" },	/* a control character that does not exist */
	};

	for (i = 0; i < sizeof replace / sizeof replace[0]; ++i) {
		struct termios t, before;
		char *text = malloc(strlen(good) + 64), *at;

		strcpy(text, good);
		at = strstr(text, replace[i][0]);
		if (!at) {
			fprintf(stderr, "no %s in <%s>\n", replace[i][0], good);
			abort();
		}
		memmove(at + strlen(replace[i][1]), at + strlen(replace[i][0]), strlen(at + strlen(replace[i][0])) + 1);
		memcpy(at, replace[i][1], strlen(replace[i][1]));
		memset(&t, 0x5a, sizeof t);
		before = t;
		assert(!ENCODE(text, &t));
		assert(memcmp(&before, &t, sizeof t) == 0);
		free(text);
	}

	/* too few fields, too many, and none */
	{
		char *cut = strdup(good), *many = malloc(strlen(good) + 16), *bang;
		struct termios t, before;

		bang = strrchr(cut, '!');
		*bang = '\0';
		memset(&t, 0x5a, sizeof t);
		before = t;
		assert(!ENCODE(cut, &t));
		assert(memcmp(&before, &t, sizeof t) == 0);

		strcpy(many, good);
		strcat(many, "!extra");
		assert(!ENCODE(many, &t));
		assert(memcmp(&before, &t, sizeof t) == 0);
		free(cut);
		free(many);
	}
	{
		char empty[] = "";
		struct termios t, before;

		memset(&t, 0x5a, sizeof t);
		before = t;
		assert(!ENCODE(empty, &t));
		assert(memcmp(&before, &t, sizeof t) == 0);
	}
	free(good);
}

/* What the peer sent is in the error text, whatever its length. */
static void scenario_long_names(void)
{
	char *good = good_string();
	size_t i;
	static const char *const names[] = { "BRKINT", "VERASE", "B1200" };

	for (i = 0; i < 3; ++i) {
		size_t extra = 300000;
		char *text = malloc(strlen(good) + extra + 16), *at;
		struct termios t;

		strcpy(text, good);
		at = strstr(text, names[i]);
		assert(at);
		memmove(at + extra, at + strlen(names[i]), strlen(at + strlen(names[i])) + 1);
		memset(at, 'Z', extra);
		memset(&t, 0, sizeof t);
		(void)ENCODE(text, &t);
		free(text);
	}
	free(good);
}

/* The text of an error is not a format. */
static void scenario_format_characters(void)
{
	char *good = good_string();
	char *text = malloc(strlen(good) + 64), *at;
	struct termios t;

	strcpy(text, good);
	at = strstr(text, "BRKINT");
	assert(at);
	memmove(at + 12, at + 6, strlen(at + 6) + 1);
	memcpy(at, "%s%s%n%n%d%d", 12);
	memset(&t, 0, sizeof t);
	(void)ENCODE(text, &t);
	free(text);
	free(good);
}

int main(int argc, char **argv)
{
	static const struct {
		const char *name;
		void (*run)(void);
	} scenarios[] = {
		{ "round_trip", scenario_round_trip },
		{ "refusals", scenario_refusals },
		{ "long_names", scenario_long_names },
		{ "format_characters", scenario_format_characters },
	};
	size_t i;

	if (argc == 2 && strcmp(argv[1], "--list") == 0) {
		for (i = 0; i < sizeof scenarios / sizeof scenarios[0]; ++i)
			puts(scenarios[i].name);
		return 0;
	}
	assert(argc == 2);
	for (i = 0; i < sizeof scenarios / sizeof scenarios[0]; ++i) {
		if (strcmp(argv[1], scenarios[i].name) == 0) {
			scenarios[i].run();
			return 0;
		}
	}
	fprintf(stderr, "unknown scenario %s\n", argv[1]);
	return 2;
}
