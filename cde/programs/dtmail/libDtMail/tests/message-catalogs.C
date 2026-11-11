// Program of tests/check-message-catalogs.py, run under AddressSanitizer and
// UndefinedBehaviorSanitizer next to the production Common/DtMailError.C.
//
//   message-catalogs dump CATALOG MAXSET MAXMSG
//       print "SET MSG HEX" for every message that the compiled catalog CATALOG
//       (a file written by gencat) holds, HEX being the bytes of the text
//   message-catalogs errors EXPECTED
//       look every error up through DtMailEnv (the code that dtmail and dtmailpr
//       run) and compare the text with the lines "NAME HEX" of EXPECTED; NAME is
//       an enumerator of DTMailError_t or "out-of-range"
//
// The enumerators come from error-names.inc, which the driver writes from
// include/DtMail/DtMailError.hh: a name that the header no longer has does not
// compile.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <nl_types.h>
#include <string>
#include <DtMail/DtMailError.hh>

namespace {

struct Name {
    const char *name;
    DTMailError_t code;
};

const Name names[] = {
#include "error-names.inc"
};

std::string hex(const char *text)
{
    static const char digits[] = "0123456789abcdef";
    std::string out;
    for (const unsigned char *p = (const unsigned char *) text; *p; p++) {
        out += digits[*p >> 4];
        out += digits[*p & 15];
    }
    return out;
}

int dump(const char *path, int max_set, int max_message)
{
    nl_catd catalog = catopen(path, 0);
    if (catalog == (nl_catd) -1) {
        fprintf(stderr, "cannot open %s\n", path);
        return 1;
    }
    static const char missing[] = "missing";
    for (int set = 1; set <= max_set; set++) {
        for (int message = 1; message <= max_message; message++) {
            const char *text = catgets(catalog, set, message, missing);
            if (text != missing)
                printf("%d %d %s\n", set, message, hex(text).c_str());
        }
    }
    catclose(catalog);
    return 0;
}

int errors(const char *path)
{
    FILE *expected = fopen(path, "r");
    if (!expected) {
        perror(path);
        return 1;
    }
    char name[256], want[8192];
    int failures = 0, checked = 0;
    while (fscanf(expected, "%255s %8191s", name, want) == 2) {
        DtMailEnv env;
        if (strcmp(name, "out-of-range") == 0) {
            // The first value past the table and the last value that the enumeration
            // can hold (a larger one is not a DTMailError_t) both read as "invalid error".
            unsigned top = DTME_MAXDTME;
            top |= top >> 1; top |= top >> 2; top |= top >> 4; top |= top >> 8; top |= top >> 16;
            DtMailEnv beyond;
            beyond.setError((DTMailError_t) top);
            std::string beyond_text = hex((const char *) beyond);
            env.setError(DTME_MAXDTME);
            std::string text = hex((const char *) env);
            checked++;
            if (text != want || beyond_text != want) {
                failures++;
                printf("%s: got %s and %s, expected %s\n", name, text.c_str(), beyond_text.c_str(), want);
            }
            continue;
        }
        const Name *found = NULL;
        for (const Name &candidate : names)
            if (strcmp(candidate.name, name) == 0)
                found = &candidate;
        if (!found) {
            printf("%s: not an error of DtMail/DtMailError.hh\n", name);
            failures++;
            continue;
        }
        env.setError(found->code);
        std::string text = hex((const char *) env);
        checked++;
        if (text != want) {
            failures++;
            printf("%s: got %s, expected %s\n", name, text.c_str(), want);
        }
    }
    fclose(expected);
    if (checked != (int) (sizeof names / sizeof names[0]) + 1) {
        printf("checked %d errors, the header has %d\n", checked, (int) (sizeof names / sizeof names[0]));
        failures++;
    }
    return failures ? 1 : 0;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc == 5 && strcmp(argv[1], "dump") == 0)
        return dump(argv[2], atoi(argv[3]), atoi(argv[4]));
    if (argc == 3 && strcmp(argv[1], "errors") == 0)
        return errors(argv[2]);
    fprintf(stderr, "usage: message-catalogs dump CATALOG MAXSET MAXMSG | errors EXPECTED\n");
    return 2;
}
