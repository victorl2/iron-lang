/* Phase 4 Plan 04-02 Task 01 (EDIT-06, D-01, D-16) -- completion context
 * classifier implementation.
 *
 * Byte-walk-backward over the document from `byte_offset`. No analyzer,
 * no arena, no allocation. Mirrors the pattern in
 * src/lsp/facade/signature_help.c: we examine raw UTF-8 bytes and look
 * for trigger characters / keyword prefixes.
 *
 * Order of classification (first match wins, falling through to
 * ILSP_CCTX_EXPR_HEAD if nothing matches):
 *   1. Import-path continuation (last keyword on line is `import`)
 *   2. Member access: last non-space byte before cursor is `.`
 *   3. Qualified path: two non-space bytes before cursor are `::`
 *   4. Type position: last non-space byte before cursor is `:` AND the
 *      preceding token looks like a decl/param ident
 *   5. Pattern position: enclosing balanced bracket scope is `match v { ... }`
 *   6. Statement head: line start (possibly after whitespace) OR line
 *      terminator immediately before cursor
 *   7. Expression head: default fallback
 */

#include "lsp/facade/edit/complete/context_classify.h"
#include "lsp/store/document.h"
#include "keyword_mirror.h"

#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* ── Internal helpers (pure byte math) ─────────────────────────────── */

static bool is_ident_start(int c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

static bool is_ident_cont(int c) {
    return is_ident_start(c) || (c >= '0' && c <= '9');
}

/* Walk backward over whitespace (but NOT across newlines unless
 * `cross_newline` is true). Returns the new offset: bytes[off] is the
 * first non-whitespace byte at or before the original cursor position.
 * If the entire prefix is whitespace, returns 0. */
static size_t skip_ws_back(const char *buf, size_t off, bool cross_newline) {
    while (off > 0) {
        char c = buf[off - 1];
        if (c == '\n' || c == '\r') {
            if (!cross_newline) return off;
            off--;
            continue;
        }
        if (isspace((unsigned char)c)) { off--; continue; }
        break;
    }
    return off;
}

/* Walk backward over an identifier starting at offset-1 (i.e. the byte
 * immediately before the cursor). Returns the offset of the first byte
 * of the identifier (or `off` unchanged if the byte before is not an
 * identifier character). */
static size_t walk_ident_back(const char *buf, size_t off) {
    while (off > 0 && is_ident_cont((unsigned char)buf[off - 1])) off--;
    return off;
}

/* True iff bytes[off..off+len] equals `kw`. */
static bool buf_matches_kw(const char *buf, size_t off, size_t len,
                            const char *kw) {
    size_t kl = strlen(kw);
    if (kl != len) return 0;
    return memcmp(buf + off, kw, kl) == 0;
}

/* True iff the ident [off..off+len) matches any keyword in the generated
 * mirror list. */
static bool is_keyword_range(const char *buf, size_t off, size_t len) {
    for (size_t i = 0; i < ILSP_COMPLETION_KEYWORD_COUNT; i++) {
        if (buf_matches_kw(buf, off, len, ILSP_COMPLETION_KEYWORDS[i])) return 1;
    }
    return 0;
}

/* Return the offset of the start of the current line (scan backwards to
 * previous '\n' or to 0). */
static size_t line_start(const char *buf, size_t off) {
    while (off > 0 && buf[off - 1] != '\n') off--;
    return off;
}

/* Find the last keyword token on the current line (from line_start up to
 * `off`). If found, set *kw_start / *kw_end to its byte range and return
 * true. Only the FIRST keyword from the back is returned.
 *
 * Skips strings ("..."), comments (// ...), and nested identifiers. */
static bool last_kw_on_line(const char *buf, size_t ls, size_t off,
                              size_t *kw_start, size_t *kw_end) {
    /* Simple linear scan: step through the line's tokens, remember the
     * last keyword seen. Strings / line comments stop the scan (well,
     * they shouldn't contain keywords anyway; we simply skip them so
     * `"import"` inside a string isn't mistaken for the keyword). */
    size_t i = ls;
    size_t best_start = 0, best_end = 0;
    bool found = false;
    while (i < off) {
        char c = buf[i];
        if (c == '"') {
            /* Skip a string literal. */
            i++;
            while (i < off && buf[i] != '"') {
                if (buf[i] == '\\' && i + 1 < off) i++;
                i++;
            }
            if (i < off) i++;  /* consume closing quote */
            continue;
        }
        if (c == '/' && i + 1 < off && buf[i + 1] == '/') {
            /* Line comment -- skip to newline. */
            while (i < off && buf[i] != '\n') i++;
            continue;
        }
        if (is_ident_start((unsigned char)c)) {
            size_t s = i;
            while (i < off && is_ident_cont((unsigned char)buf[i])) i++;
            if (is_keyword_range(buf, s, i - s)) {
                best_start = s;
                best_end   = i;
                found      = true;
            }
            continue;
        }
        i++;
    }
    if (found) {
        *kw_start = best_start;
        *kw_end   = best_end;
    }
    return found;
}

/* Return the byte offset of the beginning of the innermost `{` that
 * brackets the cursor (balanced-bracket backward scan). Returns SIZE_MAX
 * if no enclosing `{`. Also counts `(` / `[` balance so we don't cross
 * a call or subscript. */
static size_t enclosing_open_brace(const char *buf, size_t off) {
    int depth_brace = 0, depth_paren = 0, depth_bracket = 0;
    size_t i = off;
    while (i > 0) {
        char c = buf[i - 1];
        /* Walk over string literals first (they may contain braces). */
        if (c == '"') {
            /* Consume backward to the matching quote. */
            size_t j = i - 2;
            bool matched = false;
            while (j + 1 > 0 && j < off) {
                if (buf[j] == '"' && (j == 0 || buf[j - 1] != '\\')) {
                    matched = true;
                    break;
                }
                if (j == 0) break;
                j--;
            }
            if (matched) { i = j; continue; }
            /* Malformed string -- fall through treating as literal. */
        }
        switch (c) {
            case '}': depth_brace++; break;
            case ']': depth_bracket++; break;
            case ')': depth_paren++; break;
            case '{':
                if (depth_brace == 0) return i - 1;
                depth_brace--;
                break;
            case '[':
                if (depth_bracket > 0) depth_bracket--;
                break;
            case '(':
                if (depth_paren > 0) depth_paren--;
                break;
            default: break;
        }
        i--;
    }
    return (size_t)-1;
}

/* Determine whether the enclosing-brace's introducer is `match <expr>`.
 * Token-aware backward walk: alternate between consuming one token
 * (balanced `()`/`[]` or an ident or a punctuator) and separator
 * whitespace. After each ident token, check whether it's `match` — if
 * so, we found a match-scope. If we run out of tokens or hit a hard
 * statement terminator, bail out. */
static bool is_match_scope(const char *buf, size_t brace_open) {
    size_t i = skip_ws_back(buf, brace_open, true);
    if (i == 0) return 0;

    /* Limit the walk: we only need to inspect the immediate scrutinee +
     * the keyword that introduces it. At most ~8 tokens is ample. */
    int tokens_seen = 0;
    while (i > 0 && tokens_seen < 16) {
        char c = buf[i - 1];

        /* Balanced close-bracket: consume the whole `(...)` / `[...]`. */
        if (c == ')' || c == ']') {
            char open_ch = (c == ')') ? '(' : '[';
            int depth = 1;
            i--;
            while (i > 0 && depth > 0) {
                char d = buf[i - 1];
                if (d == c) depth++;
                else if (d == open_ch) depth--;
                i--;
                if (depth == 0) break;
            }
            tokens_seen++;
            i = skip_ws_back(buf, i, true);
            continue;
        }

        /* Field access dot or qualified colon -- just a separator, walk
         * over and count as part of the scrutinee chain. */
        if (c == '.' || c == ':') {
            i--;
            i = skip_ws_back(buf, i, true);
            continue;
        }

        /* Identifier token: consume, check keyword. */
        if (is_ident_cont((unsigned char)c)) {
            size_t end = i;
            size_t start = walk_ident_back(buf, end);
            /* If this ident IS `match` we found the introducer. */
            if (buf_matches_kw(buf, start, end - start, "match")) {
                /* Word boundary on the left. */
                if (start == 0 || !is_ident_cont((unsigned char)buf[start - 1])) {
                    return true;
                }
            }
            /* Otherwise treat it as scrutinee tokens; keep walking back
             * to find the keyword on its left. */
            i = start;
            tokens_seen++;
            i = skip_ws_back(buf, i, true);
            continue;
        }

        /* Anything else (operator, comma, brace, semicolon) means we've
         * walked out of the scrutinee expression without finding match. */
        break;
    }
    return false;
}

/* ── Public API ───────────────────────────────────────────────────── */

IronLsp_CompletionContext ilsp_completion_context_classify_buf(
    const char *buf, size_t len, size_t byte_offset) {
    if (!buf) return ILSP_CCTX_EXPR_HEAD;
    if (byte_offset > len) byte_offset = len;

    /* 0. If the cursor is INSIDE an identifier, walk back past it so we
     *    can inspect the trigger before the ident the user is typing. */
    size_t cur = byte_offset;
    while (cur > 0 && is_ident_cont((unsigned char)buf[cur - 1])) cur--;

    /* 1. Import path continuation: does the current line start with
     *    `import` (possibly after whitespace)?  If so, we're completing
     *    the import path regardless of trigger char. */
    size_t ls = line_start(buf, cur);
    size_t kw_s = 0, kw_e = 0;
    if (last_kw_on_line(buf, ls, cur, &kw_s, &kw_e) &&
        buf_matches_kw(buf, kw_s, kw_e - kw_s, "import")) {
        return ILSP_CCTX_IMPORT_PATH;
    }

    /* 2. Member access: the byte immediately before cur (after skipping
     *    NOTHING -- member dot bugs if we skip whitespace) is `.`. */
    if (cur > 0 && buf[cur - 1] == '.') {
        /* Guard against `..` range operator -- that is not a member
         * access. Iron's range operator is `..`. */
        if (cur < 2 || buf[cur - 2] != '.') {
            return ILSP_CCTX_MEMBER_AFTER_DOT;
        }
    }

    /* 3. Qualified path: the last two bytes before cur are `::`. */
    if (cur >= 2 && buf[cur - 1] == ':' && buf[cur - 2] == ':') {
        return ILSP_CCTX_QUALIFIED_AFTER_COLON;
    }

    /* 4. Type position: last non-whitespace-non-newline byte before cur
     *    is `:` AND the preceding identifier is the name of a val/var/
     *    func param declaration. We accept any identifier preceding `:`
     *    on the same line as a type position (Iron type annotations are
     *    always `name: Type`). */
    {
        size_t i = skip_ws_back(buf, cur, false);
        if (i > 0 && buf[i - 1] == ':') {
            /* Not `::`, already handled above. */
            if (i < 2 || buf[i - 2] != ':') {
                /* Is there an identifier preceding the `:`? */
                size_t j = skip_ws_back(buf, i - 1, false);
                size_t k = walk_ident_back(buf, j);
                if (k < j) {
                    /* Check the identifier isn't itself a keyword like
                     * `else:` (Iron doesn't have that form, but be
                     * defensive). */
                    if (!is_keyword_range(buf, k, j - k)) {
                        return ILSP_CCTX_TYPE_POSITION;
                    }
                }
            }
        }
    }

    /* 5. Pattern position: cursor is inside a `match <expr> { ... }`. */
    {
        size_t brace = enclosing_open_brace(buf, cur);
        if (brace != (size_t)-1 && is_match_scope(buf, brace)) {
            return ILSP_CCTX_PATTERN_POSITION;
        }
    }

    /* 6. Statement head: the effective line (after stripping leading
     *    whitespace) is empty before cur, OR the previous non-space byte
     *    is a newline or `;` (a statement terminator). */
    {
        size_t i = skip_ws_back(buf, cur, false);
        if (i == ls) {
            /* Only whitespace before cur on this line. */
            return ILSP_CCTX_STATEMENT_HEAD;
        }
        if (i > 0) {
            char c = buf[i - 1];
            if (c == ';') return ILSP_CCTX_STATEMENT_HEAD;
        }
    }

    /* 7. Default: expression-head context. */
    return ILSP_CCTX_EXPR_HEAD;
}

IronLsp_CompletionContext ilsp_completion_context_classify(
    const struct IronLsp_Document *doc, size_t byte_offset) {
    if (!doc || !doc->text) return ILSP_CCTX_EXPR_HEAD;
    return ilsp_completion_context_classify_buf(doc->text, doc->text_len,
                                                 byte_offset);
}
