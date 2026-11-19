#ifndef DTPAD_PAD_SYNTAX_H
#define DTPAD_PAD_SYNTAX_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DTPAD_SYNTAX_NONE = 0,
    DTPAD_SYNTAX_C,
    DTPAD_SYNTAX_SHELL,
    DTPAD_SYNTAX_JSON,
    DTPAD_SYNTAX_JS,
    DTPAD_SYNTAX_PYTHON,
    DTPAD_SYNTAX_YAML_INI,
    DTPAD_SYNTAX_MARKUP,
    DTPAD_SYNTAX_CSS,
    DTPAD_SYNTAX_MARKDOWN,
    DTPAD_SYNTAX_MAKEFILE,
    DTPAD_SYNTAX_RUST_GO,
    DTPAD_SYNTAX_SQL,
    DTPAD_SYNTAX_DIFF,
} DtpadSyntaxLang;

typedef enum {
    DTPAD_TOK_KEYWORD = 0,
    DTPAD_TOK_TYPE,
    DTPAD_TOK_STRING,
    DTPAD_TOK_NUMBER,
    DTPAD_TOK_COMMENT,
    DTPAD_TOK_PREPROC,
    DTPAD_TOK_FUNCTION,
    DTPAD_TOK_KEY,
    DTPAD_TOK_COUNT,
} DtpadSyntaxTokenKind;

typedef struct {
    long start_pos; /* UTF-8 Unicode scalar index (XmTextPosition) */
    long end_pos;   /* exclusive UTF-8 Unicode scalar index */
    DtpadSyntaxTokenKind kind;
} DtpadSyntaxSpan;

/* Detect syntax language from filename/extension and/or first-line shebang/magic. */
DtpadSyntaxLang dtpad_syntax_detect(const char *file_name, const char *text);

/* Stable short identifier ("none", "c", "shell", "json", "javascript", ...). */
const char *dtpad_syntax_lang_id(DtpadSyntaxLang lang);

/*
 * Tokenize UTF-8 buffer into non-overlapping sorted spans.
 * Caller owns *out_spans (free with free()). Returns span count (>= 0).
 */
size_t dtpad_syntax_lex(DtpadSyntaxLang lang,
                        const char *text,
                        size_t byte_len,
                        DtpadSyntaxSpan **out_spans);

#ifdef __cplusplus
}
#endif

#endif /* DTPAD_PAD_SYNTAX_H */
