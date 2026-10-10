/* layout.c -- see layout.h. */

#include "fmt/layout.h"

#include "util/strbuf.h"
#include "vendor/stb_ds.h"

#include <stdbool.h>
#include <string.h>

/* Where a scan stands inside the source. Brackets count only in plain
 * code: an interpolation `{...}` lives inside a string literal. */
enum { CTX_CODE, CTX_STR, CTX_TSTR, CTX_INTERP };

typedef struct {
    int kind;
    int depth;  /* CTX_INTERP: open braces */
} ScanCtx;

typedef struct {
    size_t start, len;      /* the line, without its '\n' */
    bool   starts_in_str;   /* first byte is inside a string literal */
    bool   ends_in_str;     /* the '\n' is inside a string literal */
    char  *brackets;        /* stb_ds: the code brackets, in order */
} Line;

typedef struct {
    int indent;             /* indent of the line that opened it */
} Open;

/* Scans the source once, splitting it into lines and recording, for each,
 * the brackets of plain code and whether it begins or ends inside a
 * string literal. */
static Line *scan_lines(const char *src) {
    Line *lines = NULL;
    ScanCtx *stack = NULL;
    ScanCtx code = { CTX_CODE, 0 };
    arrput(stack, code);

    size_t n = strlen(src);
    size_t i = 0;
    Line cur = { 0, 0, false, false, NULL };
    bool in_comment = false;

#define TOP (stack[arrlen(stack) - 1])
#define IN_STRING() (TOP.kind != CTX_CODE)
    while (i <= n) {
        char c = i < n ? src[i] : '\0';
        if (c == '\n' || c == '\0') {
            cur.len = i - cur.start;
            cur.ends_in_str = IN_STRING();
            arrput(lines, cur);
            if (c == '\0') break;
            /* A one-line string ends at the newline (the lexer reports
             * it); an interpolation inside one does too. */
            while (arrlen(stack) > 1 &&
                   (TOP.kind == CTX_STR ||
                    (TOP.kind == CTX_INTERP &&
                     stack[arrlen(stack) - 2].kind == CTX_STR)))
                arrpop(stack);
            in_comment = false;
            i++;
            cur.start = i;
            cur.len = 0;
            cur.starts_in_str = IN_STRING();
            cur.ends_in_str = false;
            cur.brackets = NULL;
            continue;
        }
        if (in_comment) { i++; continue; }

        switch (TOP.kind) {
        case CTX_CODE:
            if (c == '-' && i + 1 < n && src[i + 1] == '-') {
                in_comment = true;
                i += 2;
                continue;
            }
            if (c == '/' && i + 2 < n && src[i + 1] == '/' && src[i + 2] == '/') {
                in_comment = true;
                i += 3;
                continue;
            }
            if (c == '"') {
                bool triple = i + 2 < n && src[i + 1] == '"' && src[i + 2] == '"';
                ScanCtx s = { triple ? CTX_TSTR : CTX_STR, 0 };
                arrput(stack, s);
                i += triple ? 3 : 1;
                continue;
            }
            if (c == '(' || c == '[' || c == '{' || c == ')' || c == ']' || c == '}')
                arrput(cur.brackets, c);
            i++;
            continue;

        case CTX_STR:
        case CTX_TSTR:
            if (c == '\\') { i += (i + 1 < n && src[i + 1] != '\n') ? 2 : 1; continue; }
            if (c == '{') {
                ScanCtx s = { CTX_INTERP, 1 };
                arrput(stack, s);
                i++;
                continue;
            }
            if (c == '"') {
                if (TOP.kind == CTX_STR) { arrpop(stack); i++; continue; }
                if (i + 2 < n && src[i + 1] == '"' && src[i + 2] == '"') {
                    arrpop(stack);
                    i += 3;
                    continue;
                }
            }
            i++;
            continue;

        case CTX_INTERP:
            if (c == '\\' && i + 1 < n && src[i + 1] == '"') {
                /* `{f(\"a\")}`: a nested literal up to the next `\"`. */
                i += 2;
                while (i < n && src[i] != '\n' &&
                       !(src[i] == '\\' && i + 1 < n && src[i + 1] == '"'))
                    i++;
                if (i < n && src[i] == '\\') i += 2;
                continue;
            }
            if (c == '\\') { i += (i + 1 < n && src[i + 1] != '\n') ? 2 : 1; continue; }
            if (c == '"') {
                /* A literal nested in the expression, which may interpolate
                 * in turn: scanned like any other string. */
                ScanCtx s = { CTX_STR, 0 };
                arrput(stack, s);
                i++;
                continue;
            }
            if (c == '{') { TOP.depth++; i++; continue; }
            if (c == '}') {
                if (--TOP.depth == 0) arrpop(stack);
                i++;
                continue;
            }
            i++;
            continue;
        }
    }
#undef TOP
#undef IN_STRING
    arrfree(stack);
    return lines;
}

static bool is_closer(char c) { return c == ')' || c == ']' || c == '}'; }

char *iron_fmt_layout(const char *src, const IronFmtOptions *opts,
                      Iron_Arena *arena, uint32_t first_line, uint32_t last_line) {
    if (!src || !arena) return NULL;
    int width = (opts && opts->indent_width > 0) ? opts->indent_width : 4;
    bool tabs = opts && opts->use_tabs;
    bool whole = first_line == 0;

    Line *lines = scan_lines(src);
    int count = (int)arrlen(lines);
    /* A source ending in '\n' leaves an empty last line: not a line. */
    if (count > 0 && lines[count - 1].len == 0 && !lines[count - 1].starts_in_str &&
        strlen(src) > 0 && src[strlen(src) - 1] == '\n')
        count--;
    if (whole) { first_line = 1; last_line = (uint32_t)count; }

    Open *opens = NULL;
    Iron_StrBuf out = iron_strbuf_create(strlen(src) + 64);
    int pending_blank = 0;   /* blank lines seen since the last text line */
    bool wrote_text = false;

    for (int li = 0; li < count; li++) {
        Line *ln = &lines[li];
        const char *text = src + ln->start;
        size_t len = ln->len;
        bool emit = (uint32_t)(li + 1) >= first_line && (uint32_t)(li + 1) <= last_line;

        if (ln->starts_in_str) {
            /* Inside a multi-line string: the bytes are the value. */
            if (emit) {
                for (; pending_blank > 0; pending_blank--) iron_strbuf_appendf(&out, "\n");
                iron_strbuf_append(&out, text, len);
                iron_strbuf_appendf(&out, "\n");
                wrote_text = true;
            }
            for (int b = 0; b < (int)arrlen(ln->brackets); b++) {
                char c = ln->brackets[b];
                if (is_closer(c)) {
                    if (arrlen(opens) > 0) arrpop(opens);
                } else {
                    Open o = { arrlen(opens) > 0 ? opens[arrlen(opens) - 1].indent + 1 : 0 };
                    arrput(opens, o);
                }
            }
            continue;
        }

        size_t lead = 0;
        while (lead < len && (text[lead] == ' ' || text[lead] == '\t')) lead++;
        size_t end = len;
        if (!ln->ends_in_str)
            while (end > lead && (text[end - 1] == ' ' || text[end - 1] == '\t' ||
                                  text[end - 1] == '\r'))
                end--;

        if (end == lead) {
            if (emit) pending_blank++;
            continue;
        }

        /* Closers that begin the line take the indent of the line that
         * opened the outermost of them; anything else sits one level
         * inside the innermost open bracket. */
        int indent = arrlen(opens) > 0 ? opens[arrlen(opens) - 1].indent + 1 : 0;
        int leading = 0;
        {
            size_t p = lead;
            int nb = (int)arrlen(ln->brackets);
            while (p < end && leading < nb && is_closer(text[p]) &&
                   text[p] == ln->brackets[leading]) {
                leading++;
                p++;
                while (p < end && (text[p] == ' ' || text[p] == '\t')) p++;
            }
        }
        if (leading > 0) {
            int depth = (int)arrlen(opens);
            int at = depth - leading;
            if (at < 0) at = 0;
            indent = depth > 0 ? opens[at].indent : 0;
        }

        if (emit) {
            /* A run of blank lines becomes one; the file starts with text. */
            if (pending_blank > 0 && (wrote_text || !whole))
                iron_strbuf_appendf(&out, "\n");
            pending_blank = 0;
            for (int k = 0; k < indent; k++) {
                if (tabs) iron_strbuf_appendf(&out, "\t");
                else for (int s = 0; s < width; s++) iron_strbuf_appendf(&out, " ");
            }
            iron_strbuf_append(&out, text + lead, end - lead);
            iron_strbuf_appendf(&out, "\n");
            wrote_text = true;
        }

        /* A bracket opened after this line closed one from an earlier
         * line (`b: Int) -> Int {` ending a wrapped parameter list) belongs
         * to the statement that earlier line started. */
        int base = indent;
        for (int b = 0; b < (int)arrlen(ln->brackets); b++) {
            char c = ln->brackets[b];
            if (is_closer(c)) {
                if (arrlen(opens) > 0) {
                    int was = opens[arrlen(opens) - 1].indent;
                    arrpop(opens);
                    if (was < base) base = was;
                }
            } else {
                Open o = { base };
                arrput(opens, o);
            }
        }
    }
    /* Range mode keeps blank lines that end the range; the whole file
     * drops them. */
    if (!whole) for (; pending_blank > 0; pending_blank--) iron_strbuf_appendf(&out, "\n");

    for (int li = 0; li < (int)arrlen(lines); li++) arrfree(lines[li].brackets);
    arrfree(lines);
    arrfree(opens);

    size_t olen = out.len;
    char *res = (char *)iron_arena_alloc(arena, olen + 1, 1);
    if (res) {
        memcpy(res, iron_strbuf_get(&out), olen);
        res[olen] = '\0';
    }
    iron_strbuf_free(&out);
    return res;
}
