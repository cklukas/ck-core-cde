#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "padSyntax.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define DTPAD_SYNTAX_MAX_BYTES (512U * 1024U)
#define DTPAD_SYNTAX_MAX_SPANS 32768U

typedef struct {
    DtpadSyntaxSpan *items;
    size_t count;
    size_t cap;
} SpanBuf;

static void span_push(SpanBuf *sb, long s, long e, DtpadSyntaxTokenKind kind)
{
    if (!sb || s >= e || sb->count >= DTPAD_SYNTAX_MAX_SPANS)
        return;
    if (sb->count > 0 &&
        sb->items[sb->count - 1].end_pos == s &&
        sb->items[sb->count - 1].kind == kind) {
        sb->items[sb->count - 1].end_pos = e;
        return;
    }
    if (sb->count == sb->cap) {
        size_t ncap = sb->cap ? sb->cap * 2U : 256U;
        if (ncap > DTPAD_SYNTAX_MAX_SPANS)
            ncap = DTPAD_SYNTAX_MAX_SPANS;
        DtpadSyntaxSpan *ni = (DtpadSyntaxSpan *)realloc(sb->items, ncap * sizeof(*ni));
        if (!ni)
            return;
        sb->items = ni;
        sb->cap = ncap;
    }
    sb->items[sb->count++] = (DtpadSyntaxSpan){ s, e, kind };
}

static inline int is_ident_start(unsigned char c)
{
    return isalpha(c) || c == '_';
}

static inline int is_ident_char(unsigned char c)
{
    return isalnum(c) || c == '_';
}

/* Advance 1 UTF-8 scalar and increment *scalar_pos by 1. */
static inline size_t utf8_step(const char *s, size_t i, size_t n, long *scalar_pos)
{
    if (i >= n)
        return n;
    unsigned char c = (unsigned char)s[i++];
    if (c >= 0x80U) {
        while (i < n && (((unsigned char)s[i] & 0xC0U) == 0x80U))
            ++i;
    }
    ++(*scalar_pos);
    return i;
}

static int word_in_list(const char *w, size_t len, const char *const *list)
{
    for (size_t i = 0; list[i] != NULL; ++i) {
        if (strlen(list[i]) == len && memcmp(w, list[i], len) == 0)
            return 1;
    }
    return 0;
}

static int word_in_list_ci(const char *w, size_t len, const char *const *list)
{
    for (size_t i = 0; list[i] != NULL; ++i) {
        if (strlen(list[i]) == len && strncasecmp(w, list[i], len) == 0)
            return 1;
    }
    return 0;
}

const char *dtpad_syntax_lang_id(DtpadSyntaxLang lang)
{
    switch (lang) {
    case DTPAD_SYNTAX_C:        return "c";
    case DTPAD_SYNTAX_SHELL:    return "shell";
    case DTPAD_SYNTAX_JSON:     return "json";
    case DTPAD_SYNTAX_JS:       return "javascript";
    case DTPAD_SYNTAX_PYTHON:   return "python";
    case DTPAD_SYNTAX_YAML_INI: return "yaml";
    case DTPAD_SYNTAX_MARKUP:   return "xml";
    case DTPAD_SYNTAX_CSS:      return "css";
    case DTPAD_SYNTAX_MARKDOWN: return "markdown";
    case DTPAD_SYNTAX_MAKEFILE: return "makefile";
    case DTPAD_SYNTAX_RUST_GO:  return "rust";
    case DTPAD_SYNTAX_SQL:      return "sql";
    case DTPAD_SYNTAX_DIFF:     return "diff";
    case DTPAD_SYNTAX_NONE:
    default:                    return "none";
    }
}

static int ends_with_ci(const char *s, const char *suffix)
{
    size_t sl = strlen(s), xl = strlen(suffix);
    return sl >= xl && strcasecmp(s + sl - xl, suffix) == 0;
}

DtpadSyntaxLang dtpad_syntax_detect(const char *file_name, const char *text)
{
    if (file_name && file_name[0]) {
        const char *slash = strrchr(file_name, '/');
        const char *base = slash ? slash + 1 : file_name;

        if (strcasecmp(base, "Makefile") == 0 ||
            strcasecmp(base, "GNUmakefile") == 0 ||
            strcasecmp(base, "Imakefile") == 0 ||
            strcasecmp(base, "CMakeLists.txt") == 0 ||
            strncmp(base, "Makefile.", 9) == 0 ||
            ends_with_ci(base, ".mk") ||
            ends_with_ci(base, ".cmake")) {
            return DTPAD_SYNTAX_MAKEFILE;
        }
        if (strcmp(base, ".bashrc") == 0 ||
            strcmp(base, ".bash_profile") == 0 ||
            strcmp(base, ".profile") == 0 ||
            strcmp(base, ".zshrc") == 0 ||
            strcmp(base, ".kshrc") == 0 ||
            strcasecmp(base, "Dockerfile") == 0 ||
            ends_with_ci(base, ".sh") ||
            ends_with_ci(base, ".bash") ||
            ends_with_ci(base, ".ksh") ||
            ends_with_ci(base, ".zsh")) {
            return DTPAD_SYNTAX_SHELL;
        }
        if (ends_with_ci(base, ".c") || ends_with_ci(base, ".h") ||
            ends_with_ci(base, ".cc") || ends_with_ci(base, ".cpp") ||
            ends_with_ci(base, ".cxx") || ends_with_ci(base, ".hpp") ||
            ends_with_ci(base, ".hh") || ends_with_ci(base, ".inl") ||
            ends_with_ci(base, ".m") || ends_with_ci(base, ".mm")) {
            return DTPAD_SYNTAX_C;
        }
        if (ends_with_ci(base, ".json") || ends_with_ci(base, ".jsonc") ||
            ends_with_ci(base, ".jsonl") || ends_with_ci(base, ".webmanifest")) {
            return DTPAD_SYNTAX_JSON;
        }
        if (ends_with_ci(base, ".js") || ends_with_ci(base, ".mjs") ||
            ends_with_ci(base, ".cjs") || ends_with_ci(base, ".jsx") ||
            ends_with_ci(base, ".ts") || ends_with_ci(base, ".tsx")) {
            return DTPAD_SYNTAX_JS;
        }
        if (ends_with_ci(base, ".py") || ends_with_ci(base, ".pyw") ||
            ends_with_ci(base, ".pyi")) {
            return DTPAD_SYNTAX_PYTHON;
        }
        if (ends_with_ci(base, ".yml") || ends_with_ci(base, ".yaml") ||
            ends_with_ci(base, ".toml") || ends_with_ci(base, ".ini") ||
            ends_with_ci(base, ".conf") || ends_with_ci(base, ".cfg") ||
            ends_with_ci(base, ".desktop") || ends_with_ci(base, ".service") ||
            ends_with_ci(base, ".socket") || ends_with_ci(base, ".timer") ||
            ends_with_ci(base, ".dt") || ends_with_ci(base, ".ad") ||
            strcmp(base, ".Xresources") == 0 || strcmp(base, ".Xdefaults") == 0) {
            return DTPAD_SYNTAX_YAML_INI;
        }
        if (ends_with_ci(base, ".html") || ends_with_ci(base, ".htm") ||
            ends_with_ci(base, ".xhtml") || ends_with_ci(base, ".xml") ||
            ends_with_ci(base, ".svg") || ends_with_ci(base, ".plist") ||
            ends_with_ci(base, ".xsl") || ends_with_ci(base, ".xsd")) {
            return DTPAD_SYNTAX_MARKUP;
        }
        if (ends_with_ci(base, ".css") || ends_with_ci(base, ".scss") ||
            ends_with_ci(base, ".less")) {
            return DTPAD_SYNTAX_CSS;
        }
        if (ends_with_ci(base, ".md") || ends_with_ci(base, ".markdown") ||
            ends_with_ci(base, ".mkd")) {
            return DTPAD_SYNTAX_MARKDOWN;
        }
        if (ends_with_ci(base, ".rs") || ends_with_ci(base, ".go") ||
            ends_with_ci(base, ".java")) {
            return DTPAD_SYNTAX_RUST_GO;
        }
        if (ends_with_ci(base, ".sql")) {
            return DTPAD_SYNTAX_SQL;
        }
        if (ends_with_ci(base, ".diff") || ends_with_ci(base, ".patch")) {
            return DTPAD_SYNTAX_DIFF;
        }
    }

    if (text && text[0]) {
        if (strncmp(text, "#!", 2) == 0) {
            char line1[256];
            size_t l = 0;
            while (text[l] && text[l] != '\n' && l + 1 < sizeof(line1)) {
                line1[l] = text[l];
                ++l;
            }
            line1[l] = '\0';
            if (strstr(line1, "python"))
                return DTPAD_SYNTAX_PYTHON;
            if (strstr(line1, "node") || strstr(line1, "deno") || strstr(line1, "bun"))
                return DTPAD_SYNTAX_JS;
            if (strstr(line1, "sh") || strstr(line1, "bash") ||
                strstr(line1, "ksh") || strstr(line1, "zsh"))
                return DTPAD_SYNTAX_SHELL;
        }
        if (strncmp(text, "<?xml", 5) == 0 || strncasecmp(text, "<!DOCTYPE html", 14) == 0 ||
            strncasecmp(text, "<html", 5) == 0 || strncasecmp(text, "<svg", 4) == 0) {
            return DTPAD_SYNTAX_MARKUP;
        }
        if (strncmp(text, "diff --git ", 11) == 0 ||
            (strncmp(text, "--- ", 4) == 0 && strstr(text, "\n+++ ") != NULL)) {
            return DTPAD_SYNTAX_DIFF;
        }
    }
    return DTPAD_SYNTAX_NONE;
}

static const char *const k_c_keywords[] = {
    "if", "else", "for", "while", "do", "switch", "case", "default",
    "break", "continue", "return", "goto", "sizeof", "alignof", "typeof",
    "struct", "union", "enum", "typedef", "static", "extern", "inline",
    "const", "volatile", "restrict", "register", "auto", "constexpr",
    "class", "namespace", "template", "typename", "public", "private",
    "protected", "virtual", "override", "final", "new", "delete", "try",
    "catch", "throw", "noexcept", "using", "operator", "friend", "this",
    "true", "false", "NULL", "nullptr", "True", "False", NULL
};

static const char *const k_c_types[] = {
    "void", "char", "short", "int", "long", "float", "double", "signed",
    "unsigned", "bool", "_Bool", "size_t", "ssize_t", "ptrdiff_t",
    "intptr_t", "uintptr_t", "int8_t", "int16_t", "int32_t", "int64_t",
    "uint8_t", "uint16_t", "uint32_t", "uint64_t", "wchar_t", "FILE",
    "pid_t", "uid_t", "gid_t", "off_t", "time_t",
    "Widget", "Boolean", "Dimension", "Position", "Cardinal", "Pixel",
    "Pixmap", "XtPointer", "XtAppContext", "XmString", "XmTextPosition",
    "Display", "Window", "GC", "Atom", "Colormap", "Time", "String", NULL
};

static const char *const k_js_keywords[] = {
    "const", "let", "var", "function", "return", "if", "else", "for",
    "while", "do", "switch", "case", "default", "break", "continue",
    "try", "catch", "finally", "throw", "new", "delete", "typeof",
    "instanceof", "in", "of", "class", "extends", "super", "this",
    "import", "from", "export", "as", "async", "await", "yield",
    "static", "get", "set", "interface", "type", "enum", "implements",
    "readonly", "public", "private", "protected", "declare", "namespace",
    "true", "false", "null", "undefined", "NaN", "Infinity", NULL
};

static const char *const k_js_types[] = {
    "string", "number", "boolean", "bigint", "symbol", "object",
    "any", "unknown", "never", "void", "Promise", "Array", "Map",
    "Set", "WeakMap", "Record", "Partial", "Readonly", "Error",
    "Date", "RegExp", "JSON", "Math", "Object", "console", "window",
    "document", NULL
};

static const char *const k_rust_go_keywords[] = {
    "fn", "func", "let", "mut", "const", "static", "var", "type",
    "struct", "enum", "union", "trait", "interface", "impl", "for",
    "in", "while", "loop", "if", "else", "match", "switch", "case",
    "default", "select", "break", "continue", "return", "pub", "mod",
    "use", "crate", "super", "self", "Self", "where", "async", "await",
    "move", "ref", "unsafe", "extern", "dyn", "package", "import",
    "defer", "go", "chan", "range", "fallthrough", "map",
    "true", "false", "nil", "Some", "None", "Ok", "Err", NULL
};

static const char *const k_rust_go_types[] = {
    "i8", "i16", "i32", "i64", "i128", "isize",
    "u8", "u16", "u32", "u64", "u128", "usize",
    "f32", "f64", "bool", "char", "str", "byte", "rune", "int", "uint",
    "uintptr", "float32", "float64", "string", "error", "any",
    "String", "Vec", "Box", "Rc", "Arc", "Option", "Result", NULL
};

static const char *const k_py_keywords[] = {
    "def", "class", "return", "yield", "if", "elif", "else", "for",
    "while", "break", "continue", "pass", "import", "from", "as",
    "try", "except", "finally", "raise", "with", "assert", "del",
    "global", "nonlocal", "lambda", "and", "or", "not", "in", "is",
    "async", "await", "match", "case", "True", "False", "None", "self", NULL
};

static const char *const k_py_types[] = {
    "int", "float", "str", "bool", "bytes", "bytearray", "list", "tuple",
    "dict", "set", "frozenset", "object", "type", "Exception", "Path",
    "Optional", "Any", "Sequence", "Mapping", "Callable", NULL
};

static const char *const k_sh_keywords[] = {
    "if", "then", "elif", "else", "fi", "for", "in", "do", "done",
    "while", "until", "case", "esac", "select", "function", "time",
    "return", "exit", "break", "continue", "export", "local", "readonly",
    "declare", "typeset", "unset", "set", "shift", "source", "eval",
    "exec", "trap", "wait", "true", "false", NULL
};

static const char *const k_sql_keywords[] = {
    "SELECT", "FROM", "WHERE", "JOIN", "INNER", "LEFT", "RIGHT", "FULL",
    "OUTER", "CROSS", "ON", "USING", "AS", "DISTINCT", "ALL", "GROUP",
    "BY", "HAVING", "ORDER", "ASC", "DESC", "LIMIT", "OFFSET", "UNION",
    "INTERSECT", "EXCEPT", "INSERT", "INTO", "VALUES", "UPDATE", "SET",
    "DELETE", "CREATE", "ALTER", "DROP", "TRUNCATE", "TABLE", "VIEW",
    "INDEX", "TRIGGER", "SCHEMA", "DATABASE", "IF", "EXISTS", "NOT",
    "NULL", "PRIMARY", "KEY", "FOREIGN", "REFERENCES", "UNIQUE", "CHECK",
    "DEFAULT", "CONSTRAINT", "AND", "OR", "IN", "BETWEEN", "LIKE", "IS",
    "CASE", "WHEN", "THEN", "ELSE", "END", "CAST", "COALESCE", "BEGIN",
    "COMMIT", "ROLLBACK", "TRANSACTION", " PRAGMA", "TRUE", "FALSE", NULL
};

static const char *const k_sql_types[] = {
    "INT", "INTEGER", "SMALLINT", "BIGINT", "SERIAL", "BIGSERIAL",
    "REAL", "DOUBLE", "PRECISION", "FLOAT", "NUMERIC", "DECIMAL",
    "CHAR", "VARCHAR", "TEXT", "BLOB", "BYTEA", "BOOLEAN", "BOOL",
    "DATE", "TIME", "TIMESTAMP", "TIMESTAMPTZ", "INTERVAL", "UUID",
    "JSON", "JSONB", NULL
};

static size_t skip_quoted(const char *s, size_t i, size_t n, char quote,
                          int stop_at_newline, long *pos)
{
    i = utf8_step(s, i, n, pos); /* opening quote */
    while (i < n) {
        char c = s[i];
        if (c == '\\' && i + 1 < n) {
            i = utf8_step(s, i, n, pos);
            i = utf8_step(s, i, n, pos);
            continue;
        }
        if (stop_at_newline && c == '\n')
            break;
        i = utf8_step(s, i, n, pos);
        if (c == quote)
            break;
    }
    return i;
}

static size_t skip_number(const char *s, size_t i, size_t n, long *pos)
{
    if (s[i] == '0' && i + 1 < n &&
        (s[i + 1] == 'x' || s[i + 1] == 'X' ||
         s[i + 1] == 'b' || s[i + 1] == 'B' ||
         s[i + 1] == 'o' || s[i + 1] == 'O')) {
        i = utf8_step(s, i, n, pos);
        i = utf8_step(s, i, n, pos);
        while (i < n && (isxdigit((unsigned char)s[i]) || s[i] == '_'))
            i = utf8_step(s, i, n, pos);
    } else {
        while (i < n && (isdigit((unsigned char)s[i]) || s[i] == '_'))
            i = utf8_step(s, i, n, pos);
        if (i < n && s[i] == '.' && i + 1 < n && isdigit((unsigned char)s[i + 1])) {
            i = utf8_step(s, i, n, pos);
            while (i < n && (isdigit((unsigned char)s[i]) || s[i] == '_'))
                i = utf8_step(s, i, n, pos);
        }
        if (i < n && (s[i] == 'e' || s[i] == 'E')) {
            i = utf8_step(s, i, n, pos);
            if (i < n && (s[i] == '+' || s[i] == '-'))
                i = utf8_step(s, i, n, pos);
            while (i < n && isdigit((unsigned char)s[i]))
                i = utf8_step(s, i, n, pos);
        }
    }
    while (i < n && (s[i] == 'u' || s[i] == 'U' || s[i] == 'l' ||
                     s[i] == 'L' || s[i] == 'f' || s[i] == 'F')) {
        i = utf8_step(s, i, n, pos);
    }
    return i;
}

static void lex_clike(SpanBuf *sb, const char *s, size_t n,
                      const char *const *kw, const char *const *ty,
                      int c_preproc, int allow_backtick, int allow_decorator)
{
    size_t i = 0;
    long pos = 0;
    int at_line_start = 1;

    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\n') {
            i = utf8_step(s, i, n, &pos);
            at_line_start = 1;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\r') {
            i = utf8_step(s, i, n, &pos);
            continue;
        }
        if (c_preproc && at_line_start && c == '#') {
            long p0 = pos;
            while (i < n) {
                if (s[i] == '\\' && i + 1 < n && s[i + 1] == '\n') {
                    i = utf8_step(s, i, n, &pos);
                    i = utf8_step(s, i, n, &pos);
                    continue;
                }
                if (s[i] == '/' && i + 1 < n && (s[i + 1] == '/' || s[i + 1] == '*'))
                    break;
                if (s[i] == '\n')
                    break;
                i = utf8_step(s, i, n, &pos);
            }
            span_push(sb, p0, pos, DTPAD_TOK_PREPROC);
            at_line_start = 0;
            continue;
        }
        at_line_start = 0;

        if (!c_preproc && c == '#' && i + 1 < n && s[i + 1] == '[') {
            /* Rust #[attribute] */
            long p0 = pos;
            int depth = 0;
            i = utf8_step(s, i, n, &pos);
            while (i < n) {
                char ch = s[i];
                if (ch == '[') ++depth;
                else if (ch == ']') { --depth; i = utf8_step(s, i, n, &pos); if (depth <= 0) break; continue; }
                else if (ch == '\n' && depth <= 0) break;
                i = utf8_step(s, i, n, &pos);
            }
            span_push(sb, p0, pos, DTPAD_TOK_PREPROC);
            continue;
        }
        if (allow_decorator && c == '@' && i + 1 < n && is_ident_start((unsigned char)s[i + 1])) {
            long p0 = pos;
            i = utf8_step(s, i, n, &pos);
            while (i < n && (is_ident_char((unsigned char)s[i]) || s[i] == '.'))
                i = utf8_step(s, i, n, &pos);
            span_push(sb, p0, pos, DTPAD_TOK_PREPROC);
            continue;
        }
        if (c == '/' && i + 1 < n && s[i + 1] == '/') {
            long p0 = pos;
            while (i < n && s[i] != '\n')
                i = utf8_step(s, i, n, &pos);
            span_push(sb, p0, pos, DTPAD_TOK_COMMENT);
            continue;
        }
        if (c == '/' && i + 1 < n && s[i + 1] == '*') {
            long p0 = pos;
            i = utf8_step(s, i, n, &pos);
            i = utf8_step(s, i, n, &pos);
            while (i < n) {
                if (s[i] == '*' && i + 1 < n && s[i + 1] == '/') {
                    i = utf8_step(s, i, n, &pos);
                    i = utf8_step(s, i, n, &pos);
                    break;
                }
                i = utf8_step(s, i, n, &pos);
            }
            span_push(sb, p0, pos, DTPAD_TOK_COMMENT);
            continue;
        }
        if (c == '"' || c == '\'' || (allow_backtick && c == '`')) {
            long p0 = pos;
            i = skip_quoted(s, i, n, (char)c, c != '`', &pos);
            span_push(sb, p0, pos, DTPAD_TOK_STRING);
            continue;
        }
        if (isdigit(c)) {
            long p0 = pos;
            i = skip_number(s, i, n, &pos);
            span_push(sb, p0, pos, DTPAD_TOK_NUMBER);
            continue;
        }
        if (is_ident_start(c)) {
            size_t b0 = i;
            long p0 = pos;
            while (i < n && is_ident_char((unsigned char)s[i]))
                i = utf8_step(s, i, n, &pos);
            size_t wlen = i - b0;
            if (kw && word_in_list(s + b0, wlen, kw)) {
                span_push(sb, p0, pos, DTPAD_TOK_KEYWORD);
            } else if (ty && word_in_list(s + b0, wlen, ty)) {
                span_push(sb, p0, pos, DTPAD_TOK_TYPE);
            } else if (i < n && s[i] == '!' && !c_preproc) {
                /* Rust macro foo! */
                i = utf8_step(s, i, n, &pos);
                span_push(sb, p0, pos, DTPAD_TOK_PREPROC);
            } else {
                size_t k = i;
                while (k < n && (s[k] == ' ' || s[k] == '\t')) ++k;
                if (k < n && s[k] == '(')
                    span_push(sb, p0, pos, DTPAD_TOK_FUNCTION);
            }
            continue;
        }
        i = utf8_step(s, i, n, &pos);
    }
}

static void lex_json(SpanBuf *sb, const char *s, size_t n)
{
    size_t i = 0;
    long pos = 0;
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        if (isspace(c)) {
            i = utf8_step(s, i, n, &pos);
            continue;
        }
        if (c == '/' && i + 1 < n && s[i + 1] == '/') {
            long p0 = pos;
            while (i < n && s[i] != '\n')
                i = utf8_step(s, i, n, &pos);
            span_push(sb, p0, pos, DTPAD_TOK_COMMENT);
            continue;
        }
        if (c == '/' && i + 1 < n && s[i + 1] == '*') {
            long p0 = pos;
            i = utf8_step(s, i, n, &pos);
            i = utf8_step(s, i, n, &pos);
            while (i < n) {
                if (s[i] == '*' && i + 1 < n && s[i + 1] == '/') {
                    i = utf8_step(s, i, n, &pos);
                    i = utf8_step(s, i, n, &pos);
                    break;
                }
                i = utf8_step(s, i, n, &pos);
            }
            span_push(sb, p0, pos, DTPAD_TOK_COMMENT);
            continue;
        }
        if (c == '"') {
            long p0 = pos;
            i = skip_quoted(s, i, n, '"', 1, &pos);
            size_t k = i;
            while (k < n && (s[k] == ' ' || s[k] == '\t' || s[k] == '\r' || s[k] == '\n'))
                ++k;
            span_push(sb, p0, pos, (k < n && s[k] == ':') ? DTPAD_TOK_KEY : DTPAD_TOK_STRING);
            continue;
        }
        if (c == '-' || isdigit(c)) {
            if (c == '-' && (i + 1 >= n || !isdigit((unsigned char)s[i + 1]))) {
                i = utf8_step(s, i, n, &pos);
                continue;
            }
            long p0 = pos;
            if (c == '-')
                i = utf8_step(s, i, n, &pos);
            i = skip_number(s, i, n, &pos);
            span_push(sb, p0, pos, DTPAD_TOK_NUMBER);
            continue;
        }
        if (is_ident_start(c)) {
            size_t b0 = i;
            long p0 = pos;
            while (i < n && is_ident_char((unsigned char)s[i]))
                i = utf8_step(s, i, n, &pos);
            size_t wlen = i - b0;
            if ((wlen == 4 && (memcmp(s + b0, "true", 4) == 0 || memcmp(s + b0, "null", 4) == 0)) ||
                (wlen == 5 && memcmp(s + b0, "false", 5) == 0)) {
                span_push(sb, p0, pos, DTPAD_TOK_KEYWORD);
            }
            continue;
        }
        i = utf8_step(s, i, n, &pos);
    }
}

static void lex_shell(SpanBuf *sb, const char *s, size_t n)
{
    size_t i = 0;
    long pos = 0;
    int prev_boundary = 1;

    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\n') {
            i = utf8_step(s, i, n, &pos);
            prev_boundary = 1;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\r' || c == ';' || c == '|' || c == '&' || c == '(' || c == ')') {
            i = utf8_step(s, i, n, &pos);
            prev_boundary = 1;
            continue;
        }
        if (c == '#' && (prev_boundary || i == 0)) {
            long p0 = pos;
            DtpadSyntaxTokenKind k = (i == 0 && i + 1 < n && s[1] == '!') ? DTPAD_TOK_PREPROC : DTPAD_TOK_COMMENT;
            while (i < n && s[i] != '\n')
                i = utf8_step(s, i, n, &pos);
            span_push(sb, p0, pos, k);
            prev_boundary = 1;
            continue;
        }
        prev_boundary = 0;

        if (c == '"' || c == '\'' || c == '`') {
            long p0 = pos;
            if (c == '\'') {
                i = utf8_step(s, i, n, &pos);
                while (i < n) {
                    char ch = s[i];
                    i = utf8_step(s, i, n, &pos);
                    if (ch == '\'') break;
                }
            } else {
                i = skip_quoted(s, i, n, (char)c, 0, &pos);
            }
            span_push(sb, p0, pos, DTPAD_TOK_STRING);
            continue;
        }
        if (c == '$') {
            long p0 = pos;
            i = utf8_step(s, i, n, &pos);
            if (i < n && s[i] == '{') {
                i = utf8_step(s, i, n, &pos);
                while (i < n && s[i] != '}' && s[i] != '\n')
                    i = utf8_step(s, i, n, &pos);
                if (i < n && s[i] == '}')
                    i = utf8_step(s, i, n, &pos);
            } else if (i < n && (is_ident_start((unsigned char)s[i]) || isdigit((unsigned char)s[i]) ||
                                 strchr("?#$!@*-", s[i]) != NULL)) {
                if (is_ident_start((unsigned char)s[i])) {
                    while (i < n && is_ident_char((unsigned char)s[i]))
                        i = utf8_step(s, i, n, &pos);
                } else {
                    i = utf8_step(s, i, n, &pos);
                }
            }
            span_push(sb, p0, pos, DTPAD_TOK_PREPROC);
            continue;
        }
        if (isdigit(c)) {
            long p0 = pos;
            i = skip_number(s, i, n, &pos);
            span_push(sb, p0, pos, DTPAD_TOK_NUMBER);
            continue;
        }
        if (is_ident_start(c)) {
            size_t b0 = i;
            long p0 = pos;
            while (i < n && (is_ident_char((unsigned char)s[i]) || s[i] == '-'))
                i = utf8_step(s, i, n, &pos);
            size_t wlen = i - b0;
            if (i < n && s[i] == '=') {
                span_push(sb, p0, pos, DTPAD_TOK_KEY);
            } else if (word_in_list(s + b0, wlen, k_sh_keywords)) {
                span_push(sb, p0, pos, DTPAD_TOK_KEYWORD);
            } else {
                size_t k = i;
                while (k < n && (s[k] == ' ' || s[k] == '\t')) ++k;
                if (k + 1 < n && s[k] == '(' && s[k + 1] == ')')
                    span_push(sb, p0, pos, DTPAD_TOK_FUNCTION);
            }
            continue;
        }
        i = utf8_step(s, i, n, &pos);
    }
}

static void lex_python(SpanBuf *sb, const char *s, size_t n)
{
    size_t i = 0;
    long pos = 0;
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        if (isspace(c)) {
            i = utf8_step(s, i, n, &pos);
            continue;
        }
        if (c == '#') {
            long p0 = pos;
            DtpadSyntaxTokenKind k = (i == 0 && i + 1 < n && s[1] == '!') ? DTPAD_TOK_PREPROC : DTPAD_TOK_COMMENT;
            while (i < n && s[i] != '\n')
                i = utf8_step(s, i, n, &pos);
            span_push(sb, p0, pos, k);
            continue;
        }
        if (c == '@' && i + 1 < n && is_ident_start((unsigned char)s[i + 1])) {
            long p0 = pos;
            i = utf8_step(s, i, n, &pos);
            while (i < n && (is_ident_char((unsigned char)s[i]) || s[i] == '.'))
                i = utf8_step(s, i, n, &pos);
            span_push(sb, p0, pos, DTPAD_TOK_PREPROC);
            continue;
        }
        if ((c == '"' || c == '\'') ||
            ((c == 'f' || c == 'F' || c == 'r' || c == 'R' || c == 'b' || c == 'B') &&
             i + 1 < n && (s[i + 1] == '"' || s[i + 1] == '\''))) {
            long p0 = pos;
            if (c != '"' && c != '\'')
                i = utf8_step(s, i, n, &pos);
            char q = s[i];
            if (i + 2 < n && s[i + 1] == q && s[i + 2] == q) {
                i = utf8_step(s, i, n, &pos);
                i = utf8_step(s, i, n, &pos);
                i = utf8_step(s, i, n, &pos);
                while (i < n) {
                    if (s[i] == '\\' && i + 1 < n) {
                        i = utf8_step(s, i, n, &pos);
                        i = utf8_step(s, i, n, &pos);
                        continue;
                    }
                    if (i + 2 < n && s[i] == q && s[i + 1] == q && s[i + 2] == q) {
                        i = utf8_step(s, i, n, &pos);
                        i = utf8_step(s, i, n, &pos);
                        i = utf8_step(s, i, n, &pos);
                        break;
                    }
                    i = utf8_step(s, i, n, &pos);
                }
            } else {
                i = skip_quoted(s, i, n, q, 1, &pos);
            }
            span_push(sb, p0, pos, DTPAD_TOK_STRING);
            continue;
        }
        if (isdigit(c)) {
            long p0 = pos;
            i = skip_number(s, i, n, &pos);
            span_push(sb, p0, pos, DTPAD_TOK_NUMBER);
            continue;
        }
        if (is_ident_start(c)) {
            size_t b0 = i;
            long p0 = pos;
            while (i < n && is_ident_char((unsigned char)s[i]))
                i = utf8_step(s, i, n, &pos);
            size_t wlen = i - b0;
            if (word_in_list(s + b0, wlen, k_py_keywords)) {
                span_push(sb, p0, pos, DTPAD_TOK_KEYWORD);
            } else if (word_in_list(s + b0, wlen, k_py_types)) {
                span_push(sb, p0, pos, DTPAD_TOK_TYPE);
            } else if (i < n && s[i] == '(') {
                span_push(sb, p0, pos, DTPAD_TOK_FUNCTION);
            }
            continue;
        }
        i = utf8_step(s, i, n, &pos);
    }
}

static void lex_yaml_ini(SpanBuf *sb, const char *s, size_t n)
{
    size_t i = 0;
    long pos = 0;
    while (i < n) {
        size_t ls = i;
        while (i < n && (s[i] == ' ' || s[i] == '\t'))
            i = utf8_step(s, i, n, &pos);
        if (i >= n) break;
        if (s[i] == '\n' || s[i] == '\r') {
            i = utf8_step(s, i, n, &pos);
            continue;
        }
        if (s[i] == '#' || s[i] == ';' || (i == ls && s[i] == '!')) {
            long p0 = pos;
            while (i < n && s[i] != '\n')
                i = utf8_step(s, i, n, &pos);
            span_push(sb, p0, pos, DTPAD_TOK_COMMENT);
            continue;
        }
        if (s[i] == '[') {
            long p0 = pos;
            while (i < n && s[i] != '\n' && s[i] != ']')
                i = utf8_step(s, i, n, &pos);
            if (i < n && s[i] == ']')
                i = utf8_step(s, i, n, &pos);
            span_push(sb, p0, pos, DTPAD_TOK_TYPE);
        } else {
            if (s[i] == '-' && i + 1 < n && s[i + 1] == ' ') {
                long p0 = pos;
                i = utf8_step(s, i, n, &pos);
                span_push(sb, p0, pos, DTPAD_TOK_KEYWORD);
                while (i < n && (s[i] == ' ' || s[i] == '\t'))
                    i = utf8_step(s, i, n, &pos);
            }
            /* Check for key: or key = on this line */
            size_t probe = i;
            int found_sep = 0;
            while (probe < n && s[probe] != '\n' && s[probe] != '#') {
                if (s[probe] == '=' || (s[probe] == ':' && (probe + 1 >= n || isspace((unsigned char)s[probe + 1])))) {
                    found_sep = 1;
                    break;
                }
                if (s[probe] == '"' || s[probe] == '\'')
                    break;
                ++probe;
            }
            if (found_sep && probe > i) {
                long p0 = pos;
                while (i < probe)
                    i = utf8_step(s, i, n, &pos);
                span_push(sb, p0, pos, DTPAD_TOK_KEY);
                if (i < n && (s[i] == ':' || s[i] == '='))
                    i = utf8_step(s, i, n, &pos);
            }
        }
        /* Scan remainder of value on line */
        while (i < n && s[i] != '\n') {
            unsigned char c = (unsigned char)s[i];
            if (c == ' ' || c == '\t' || c == '\r') {
                i = utf8_step(s, i, n, &pos);
                continue;
            }
            if (c == '#') {
                long p0 = pos;
                while (i < n && s[i] != '\n')
                    i = utf8_step(s, i, n, &pos);
                span_push(sb, p0, pos, DTPAD_TOK_COMMENT);
                break;
            }
            if (c == '"' || c == '\'') {
                long p0 = pos;
                i = skip_quoted(s, i, n, (char)c, 1, &pos);
                span_push(sb, p0, pos, DTPAD_TOK_STRING);
                continue;
            }
            if (isdigit(c) || (c == '-' && i + 1 < n && isdigit((unsigned char)s[i + 1]))) {
                long p0 = pos;
                if (c == '-') i = utf8_step(s, i, n, &pos);
                i = skip_number(s, i, n, &pos);
                span_push(sb, p0, pos, DTPAD_TOK_NUMBER);
                continue;
            }
            if (is_ident_start(c)) {
                size_t b0 = i;
                long p0 = pos;
                while (i < n && is_ident_char((unsigned char)s[i]))
                    i = utf8_step(s, i, n, &pos);
                size_t wlen = i - b0;
                const char *const ykw[] = { "true", "false", "null", "yes", "no", "on", "off", NULL };
                if (word_in_list_ci(s + b0, wlen, ykw))
                    span_push(sb, p0, pos, DTPAD_TOK_KEYWORD);
                continue;
            }
            i = utf8_step(s, i, n, &pos);
        }
    }
}

static void lex_markup(SpanBuf *sb, const char *s, size_t n)
{
    size_t i = 0;
    long pos = 0;
    while (i < n) {
        if (s[i] == '<' && i + 3 < n && s[i + 1] == '!' && s[i + 2] == '-' && s[i + 3] == '-') {
            long p0 = pos;
            i = utf8_step(s, i, n, &pos);
            i = utf8_step(s, i, n, &pos);
            i = utf8_step(s, i, n, &pos);
            i = utf8_step(s, i, n, &pos);
            while (i < n) {
                if (i + 2 < n && s[i] == '-' && s[i + 1] == '-' && s[i + 2] == '>') {
                    i = utf8_step(s, i, n, &pos);
                    i = utf8_step(s, i, n, &pos);
                    i = utf8_step(s, i, n, &pos);
                    break;
                }
                i = utf8_step(s, i, n, &pos);
            }
            span_push(sb, p0, pos, DTPAD_TOK_COMMENT);
            continue;
        }
        if (s[i] == '<') {
            long p0 = pos;
            i = utf8_step(s, i, n, &pos);
            if (i < n && (s[i] == '/' || s[i] == '?' || s[i] == '!'))
                i = utf8_step(s, i, n, &pos);
            while (i < n && (isalnum((unsigned char)s[i]) || s[i] == ':' || s[i] == '-' || s[i] == '_'))
                i = utf8_step(s, i, n, &pos);
            span_push(sb, p0, pos, DTPAD_TOK_KEYWORD);
            while (i < n && s[i] != '>') {
                if (isspace((unsigned char)s[i])) {
                    i = utf8_step(s, i, n, &pos);
                    continue;
                }
                if (s[i] == '"' || s[i] == '\'') {
                    long sp0 = pos;
                    i = skip_quoted(s, i, n, s[i], 0, &pos);
                    span_push(sb, sp0, pos, DTPAD_TOK_STRING);
                    continue;
                }
                if (isalpha((unsigned char)s[i]) || s[i] == ':' || s[i] == '_') {
                    long ap0 = pos;
                    while (i < n && (isalnum((unsigned char)s[i]) || s[i] == ':' || s[i] == '-' || s[i] == '_'))
                        i = utf8_step(s, i, n, &pos);
                    span_push(sb, ap0, pos, DTPAD_TOK_KEY);
                    continue;
                }
                i = utf8_step(s, i, n, &pos);
            }
            if (i < n && s[i] == '>') {
                long ep0 = pos;
                i = utf8_step(s, i, n, &pos);
                span_push(sb, ep0, pos, DTPAD_TOK_KEYWORD);
            }
            continue;
        }
        if (s[i] == '&') {
            size_t k = i + 1;
            while (k < n && k - i < 12 && (isalnum((unsigned char)s[k]) || s[k] == '#'))
                ++k;
            if (k < n && k > i + 1 && s[k] == ';') {
                long p0 = pos;
                while (i <= k)
                    i = utf8_step(s, i, n, &pos);
                span_push(sb, p0, pos, DTPAD_TOK_PREPROC);
                continue;
            }
        }
        i = utf8_step(s, i, n, &pos);
    }
}

static void lex_css(SpanBuf *sb, const char *s, size_t n)
{
    size_t i = 0;
    long pos = 0;
    int in_block = 0;
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        if (isspace(c)) {
            i = utf8_step(s, i, n, &pos);
            continue;
        }
        if (c == '/' && i + 1 < n && s[i + 1] == '*') {
            long p0 = pos;
            i = utf8_step(s, i, n, &pos);
            i = utf8_step(s, i, n, &pos);
            while (i < n) {
                if (s[i] == '*' && i + 1 < n && s[i + 1] == '/') {
                    i = utf8_step(s, i, n, &pos);
                    i = utf8_step(s, i, n, &pos);
                    break;
                }
                i = utf8_step(s, i, n, &pos);
            }
            span_push(sb, p0, pos, DTPAD_TOK_COMMENT);
            continue;
        }
        if (c == '{') { in_block = 1; i = utf8_step(s, i, n, &pos); continue; }
        if (c == '}') { in_block = 0; i = utf8_step(s, i, n, &pos); continue; }
        if (c == '@' && !in_block) {
            long p0 = pos;
            i = utf8_step(s, i, n, &pos);
            while (i < n && (isalnum((unsigned char)s[i]) || s[i] == '-'))
                i = utf8_step(s, i, n, &pos);
            span_push(sb, p0, pos, DTPAD_TOK_PREPROC);
            continue;
        }
        if (c == '"' || c == '\'') {
            long p0 = pos;
            i = skip_quoted(s, i, n, (char)c, 1, &pos);
            span_push(sb, p0, pos, DTPAD_TOK_STRING);
            continue;
        }
        if (in_block && c == '#') {
            long p0 = pos;
            i = utf8_step(s, i, n, &pos);
            while (i < n && isxdigit((unsigned char)s[i]))
                i = utf8_step(s, i, n, &pos);
            span_push(sb, p0, pos, DTPAD_TOK_NUMBER);
            continue;
        }
        if (isdigit(c)) {
            long p0 = pos;
            i = skip_number(s, i, n, &pos);
            while (i < n && (isalpha((unsigned char)s[i]) || s[i] == '%'))
                i = utf8_step(s, i, n, &pos);
            span_push(sb, p0, pos, DTPAD_TOK_NUMBER);
            continue;
        }
        if (isalpha(c) || c == '-' || (!in_block && (c == '.' || c == '#'))) {
            long p0 = pos;
            i = utf8_step(s, i, n, &pos);
            while (i < n && (isalnum((unsigned char)s[i]) || s[i] == '-' || s[i] == '_'))
                i = utf8_step(s, i, n, &pos);
            if (!in_block) {
                span_push(sb, p0, pos, DTPAD_TOK_TYPE);
            } else {
                size_t k = i;
                while (k < n && (s[k] == ' ' || s[k] == '\t')) ++k;
                if (k < n && s[k] == ':')
                    span_push(sb, p0, pos, DTPAD_TOK_KEY);
            }
            continue;
        }
        i = utf8_step(s, i, n, &pos);
    }
}

static void lex_markdown(SpanBuf *sb, const char *s, size_t n)
{
    size_t i = 0;
    long pos = 0;
    int in_fence = 0;
    while (i < n) {
        if (strncmp(s + i, "```", 3) == 0) {
            long p0 = pos;
            while (i < n && s[i] != '\n')
                i = utf8_step(s, i, n, &pos);
            span_push(sb, p0, pos, DTPAD_TOK_PREPROC);
            in_fence = !in_fence;
            if (i < n && s[i] == '\n')
                i = utf8_step(s, i, n, &pos);
            continue;
        }
        if (in_fence) {
            long p0 = pos;
            while (i < n && s[i] != '\n')
                i = utf8_step(s, i, n, &pos);
            span_push(sb, p0, pos, DTPAD_TOK_STRING);
            if (i < n && s[i] == '\n')
                i = utf8_step(s, i, n, &pos);
            continue;
        }
        if (s[i] == '#') {
            long p0 = pos;
            while (i < n && s[i] != '\n')
                i = utf8_step(s, i, n, &pos);
            span_push(sb, p0, pos, DTPAD_TOK_KEYWORD);
            if (i < n && s[i] == '\n')
                i = utf8_step(s, i, n, &pos);
            continue;
        }
        if (s[i] == '>') {
            long p0 = pos;
            while (i < n && s[i] != '\n')
                i = utf8_step(s, i, n, &pos);
            span_push(sb, p0, pos, DTPAD_TOK_COMMENT);
            if (i < n && s[i] == '\n')
                i = utf8_step(s, i, n, &pos);
            continue;
        }
        while (i < n && s[i] != '\n') {
            if (s[i] == '`') {
                long p0 = pos;
                i = utf8_step(s, i, n, &pos);
                while (i < n && s[i] != '`' && s[i] != '\n')
                    i = utf8_step(s, i, n, &pos);
                if (i < n && s[i] == '`')
                    i = utf8_step(s, i, n, &pos);
                span_push(sb, p0, pos, DTPAD_TOK_STRING);
                continue;
            }
            if (s[i] == '[') {
                long p0 = pos;
                i = utf8_step(s, i, n, &pos);
                while (i < n && s[i] != ']' && s[i] != '\n')
                    i = utf8_step(s, i, n, &pos);
                if (i < n && s[i] == ']') {
                    i = utf8_step(s, i, n, &pos);
                    span_push(sb, p0, pos, DTPAD_TOK_TYPE);
                    if (i < n && s[i] == '(') {
                        long up0 = pos;
                        i = utf8_step(s, i, n, &pos);
                        while (i < n && s[i] != ')' && s[i] != '\n')
                            i = utf8_step(s, i, n, &pos);
                        if (i < n && s[i] == ')')
                            i = utf8_step(s, i, n, &pos);
                        span_push(sb, up0, pos, DTPAD_TOK_PREPROC);
                    }
                    continue;
                }
            }
            i = utf8_step(s, i, n, &pos);
        }
        if (i < n && s[i] == '\n')
            i = utf8_step(s, i, n, &pos);
    }
}

static void lex_makefile(SpanBuf *sb, const char *s, size_t n)
{
    static const char *const mkw[] = {
        "ifeq", "ifneq", "ifdef", "ifndef", "else", "endif", "include",
        "-include", "sinclude", "override", "export", "unexport", "define",
        "endef", "cmake_minimum_required", "project", "add_executable",
        "add_library", "target_link_libraries", "target_include_directories",
        "set", "option", "if", "elseif", "foreach", "endforeach",
        "function", "endfunction", "macro", "endmacro", "find_package", NULL
    };
    size_t i = 0;
    long pos = 0;
    while (i < n) {
        int recipe = (s[i] == '\t');
        while (i < n && (s[i] == ' ' || s[i] == '\t'))
            i = utf8_step(s, i, n, &pos);
        if (i >= n) break;
        if (s[i] == '\n') { i = utf8_step(s, i, n, &pos); continue; }
        if (s[i] == '#') {
            long p0 = pos;
            while (i < n && s[i] != '\n')
                i = utf8_step(s, i, n, &pos);
            span_push(sb, p0, pos, DTPAD_TOK_COMMENT);
            continue;
        }
        if (!recipe) {
            /* Check for target: or VAR = / := / ?= / += */
            size_t k = i;
            char sep = 0;
            while (k < n && s[k] != '\n' && s[k] != '#' && s[k] != '(') {
                if (s[k] == ':' && (k + 1 >= n || s[k + 1] != '=')) { sep = ':'; break; }
                if (s[k] == '=') { sep = '='; break; }
                ++k;
            }
            if (sep == ':' && k > i) {
                long p0 = pos;
                while (i < k) i = utf8_step(s, i, n, &pos);
                span_push(sb, p0, pos, DTPAD_TOK_FUNCTION);
            } else if (sep == '=' && k > i) {
                size_t ve = k;
                if (ve > i && (s[ve - 1] == ':' || s[ve - 1] == '?' || s[ve - 1] == '+'))
                    --ve;
                long p0 = pos;
                while (i < ve) i = utf8_step(s, i, n, &pos);
                span_push(sb, p0, pos, DTPAD_TOK_KEY);
            }
        }
        while (i < n && s[i] != '\n') {
            unsigned char c = (unsigned char)s[i];
            if (c == '#') {
                long p0 = pos;
                while (i < n && s[i] != '\n') i = utf8_step(s, i, n, &pos);
                span_push(sb, p0, pos, DTPAD_TOK_COMMENT);
                break;
            }
            if (c == '$' && i + 1 < n && (s[i + 1] == '(' || s[i + 1] == '{')) {
                char close_c = s[i + 1] == '(' ? ')' : '}';
                long p0 = pos;
                i = utf8_step(s, i, n, &pos);
                i = utf8_step(s, i, n, &pos);
                while (i < n && s[i] != close_c && s[i] != '\n')
                    i = utf8_step(s, i, n, &pos);
                if (i < n && s[i] == close_c)
                    i = utf8_step(s, i, n, &pos);
                span_push(sb, p0, pos, DTPAD_TOK_PREPROC);
                continue;
            }
            if (c == '"' || c == '\'') {
                long p0 = pos;
                i = skip_quoted(s, i, n, (char)c, 1, &pos);
                span_push(sb, p0, pos, DTPAD_TOK_STRING);
                continue;
            }
            if (is_ident_start(c) || c == '-') {
                size_t b0 = i;
                long p0 = pos;
                while (i < n && (is_ident_char((unsigned char)s[i]) || s[i] == '-'))
                    i = utf8_step(s, i, n, &pos);
                if (word_in_list_ci(s + b0, i - b0, mkw))
                    span_push(sb, p0, pos, DTPAD_TOK_KEYWORD);
                continue;
            }
            i = utf8_step(s, i, n, &pos);
        }
        if (i < n && s[i] == '\n')
            i = utf8_step(s, i, n, &pos);
    }
}

static void lex_sql(SpanBuf *sb, const char *s, size_t n)
{
    size_t i = 0;
    long pos = 0;
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        if (isspace(c)) { i = utf8_step(s, i, n, &pos); continue; }
        if (c == '-' && i + 1 < n && s[i + 1] == '-') {
            long p0 = pos;
            while (i < n && s[i] != '\n') i = utf8_step(s, i, n, &pos);
            span_push(sb, p0, pos, DTPAD_TOK_COMMENT);
            continue;
        }
        if (c == '/' && i + 1 < n && s[i + 1] == '*') {
            long p0 = pos;
            i = utf8_step(s, i, n, &pos);
            i = utf8_step(s, i, n, &pos);
            while (i < n) {
                if (s[i] == '*' && i + 1 < n && s[i + 1] == '/') {
                    i = utf8_step(s, i, n, &pos);
                    i = utf8_step(s, i, n, &pos);
                    break;
                }
                i = utf8_step(s, i, n, &pos);
            }
            span_push(sb, p0, pos, DTPAD_TOK_COMMENT);
            continue;
        }
        if (c == '\'' || c == '"') {
            long p0 = pos;
            i = skip_quoted(s, i, n, (char)c, 0, &pos);
            span_push(sb, p0, pos, DTPAD_TOK_STRING);
            continue;
        }
        if (isdigit(c)) {
            long p0 = pos;
            i = skip_number(s, i, n, &pos);
            span_push(sb, p0, pos, DTPAD_TOK_NUMBER);
            continue;
        }
        if (is_ident_start(c)) {
            size_t b0 = i;
            long p0 = pos;
            while (i < n && is_ident_char((unsigned char)s[i]))
                i = utf8_step(s, i, n, &pos);
            size_t wlen = i - b0;
            if (word_in_list_ci(s + b0, wlen, k_sql_keywords))
                span_push(sb, p0, pos, DTPAD_TOK_KEYWORD);
            else if (word_in_list_ci(s + b0, wlen, k_sql_types))
                span_push(sb, p0, pos, DTPAD_TOK_TYPE);
            continue;
        }
        i = utf8_step(s, i, n, &pos);
    }
}

static void lex_diff(SpanBuf *sb, const char *s, size_t n)
{
    size_t i = 0;
    long pos = 0;
    while (i < n) {
        long p0 = pos;
        char first = s[i];
        int has_kind = 1;
        DtpadSyntaxTokenKind kind = DTPAD_TOK_COMMENT;
        if (strncmp(s + i, "diff ", 5) == 0 || strncmp(s + i, "index ", 6) == 0 ||
            strncmp(s + i, "--- ", 4) == 0 || strncmp(s + i, "+++ ", 4) == 0) {
            kind = DTPAD_TOK_PREPROC;
        } else if (strncmp(s + i, "@@", 2) == 0) {
            kind = DTPAD_TOK_TYPE;
        } else if (first == '+') {
            kind = DTPAD_TOK_STRING;
        } else if (first == '-') {
            kind = DTPAD_TOK_NUMBER;
        } else {
            has_kind = 0;
        }
        while (i < n && s[i] != '\n')
            i = utf8_step(s, i, n, &pos);
        if (has_kind)
            span_push(sb, p0, pos, kind);
        if (i < n && s[i] == '\n')
            i = utf8_step(s, i, n, &pos);
    }
}

size_t dtpad_syntax_lex(DtpadSyntaxLang lang,
                        const char *text,
                        size_t byte_len,
                        DtpadSyntaxSpan **out_spans)
{
    if (!out_spans)
        return 0;
    *out_spans = NULL;
    if (lang == DTPAD_SYNTAX_NONE || !text || byte_len == 0)
        return 0;
    if (byte_len > DTPAD_SYNTAX_MAX_BYTES)
        byte_len = DTPAD_SYNTAX_MAX_BYTES;

    SpanBuf sb = { NULL, 0, 0 };
    switch (lang) {
    case DTPAD_SYNTAX_C:
        lex_clike(&sb, text, byte_len, k_c_keywords, k_c_types, 1, 0, 0);
        break;
    case DTPAD_SYNTAX_JS:
        lex_clike(&sb, text, byte_len, k_js_keywords, k_js_types, 0, 1, 1);
        break;
    case DTPAD_SYNTAX_RUST_GO:
        lex_clike(&sb, text, byte_len, k_rust_go_keywords, k_rust_go_types, 0, 1, 0);
        break;
    case DTPAD_SYNTAX_JSON:
        lex_json(&sb, text, byte_len);
        break;
    case DTPAD_SYNTAX_SHELL:
        lex_shell(&sb, text, byte_len);
        break;
    case DTPAD_SYNTAX_PYTHON:
        lex_python(&sb, text, byte_len);
        break;
    case DTPAD_SYNTAX_YAML_INI:
        lex_yaml_ini(&sb, text, byte_len);
        break;
    case DTPAD_SYNTAX_MARKUP:
        lex_markup(&sb, text, byte_len);
        break;
    case DTPAD_SYNTAX_CSS:
        lex_css(&sb, text, byte_len);
        break;
    case DTPAD_SYNTAX_MARKDOWN:
        lex_markdown(&sb, text, byte_len);
        break;
    case DTPAD_SYNTAX_MAKEFILE:
        lex_makefile(&sb, text, byte_len);
        break;
    case DTPAD_SYNTAX_SQL:
        lex_sql(&sb, text, byte_len);
        break;
    case DTPAD_SYNTAX_DIFF:
        lex_diff(&sb, text, byte_len);
        break;
    case DTPAD_SYNTAX_NONE:
    default:
        break;
    }

    if (sb.count == 0) {
        free(sb.items);
        return 0;
    }
    *out_spans = sb.items;
    return sb.count;
}
