#include "cli/toml.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ── Minimal iron.toml parser ────────────────────────────────────────────── */
/*
 * Supports only the fields used by Iron:
 *   [project]
 *     name    = "my_game"
 *     version = "0.1.0"
 *     entry   = "src/main.iron"
 *   [dependencies]
 *     raylib = true
 *
 * Comments: # to end of line.
 * String values: "quoted" or bare words.
 * Not a full TOML parser — just what we need.
 */

/* ── Helper: trim leading and trailing whitespace in-place ───────────────── */

static char *trim(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)*(end - 1))) end--;
    *end = '\0';
    return s;
}

/* ── Helper: strip inline comment (everything from # onwards) ───────────── */

static void strip_comment(char *s) {
    bool in_quote = false;
    for (char *p = s; *p; p++) {
        if (*p == '"') {
            in_quote = !in_quote;
        } else if (*p == '#' && !in_quote) {
            *p = '\0';
            break;
        }
    }
}

/* ── Helper: extract string value from   key = "value"  or  key = bare ──── */

static char *extract_value(const char *raw) {
    /* raw points to the value portion (after '='), already stripped */
    const char *v = raw;
    while (*v && isspace((unsigned char)*v)) v++;

    if (*v == '"') {
        /* Quoted string */
        v++;
        const char *end = strchr(v, '"');
        if (!end) return NULL;
        size_t len = (size_t)(end - v);
        char *result = (char *)malloc(len + 1);
        if (!result) return NULL;
        memcpy(result, v, len);
        result[len] = '\0';
        return result;
    } else {
        /* Bare word */
        size_t len = strlen(v);
        if (len == 0) return NULL;
        char *result = (char *)malloc(len + 1);
        if (!result) return NULL;
        memcpy(result, v, len);
        result[len] = '\0';
        return result;
    }
}

/* ── iron_toml_parse ─────────────────────────────────────────────────────── */

IronProject *iron_toml_parse(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;

    IronProject *proj = (IronProject *)calloc(1, sizeof(IronProject));
    if (!proj) {
        fclose(f);
        return NULL;
    }

    /* Track current section: 0 = none, 1 = [project], 2 = [dependencies] */
    int section = 0;

    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        /* Strip trailing newline */
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }

        /* Strip inline comment */
        strip_comment(line);

        /* Trim whitespace */
        char *s = trim(line);

        /* Skip empty lines */
        if (*s == '\0') continue;

        /* Section header: [project] or [dependencies] */
        if (*s == '[') {
            char *end = strchr(s, ']');
            if (end) {
                *end = '\0';
                char *sec_name = trim(s + 1);
                if (strcmp(sec_name, "project") == 0) {
                    section = 1;
                } else if (strcmp(sec_name, "dependencies") == 0) {
                    section = 2;
                } else {
                    section = 0;
                }
            }
            continue;
        }

        /* Key = value line */
        char *eq = strchr(s, '=');
        if (!eq) continue;

        *eq = '\0';
        char *key = trim(s);
        char *val_str = trim(eq + 1);

        if (section == 1) {
            /* [project] section */
            if (strcmp(key, "name") == 0) {
                free(proj->name);
                proj->name = extract_value(val_str);
            } else if (strcmp(key, "version") == 0) {
                free(proj->version);
                proj->version = extract_value(val_str);
            } else if (strcmp(key, "entry") == 0) {
                free(proj->entry);
                proj->entry = extract_value(val_str);
            }
        } else if (section == 2) {
            /* [dependencies] section */
            if (strcmp(key, "raylib") == 0) {
                proj->raylib = (strcmp(val_str, "true") == 0);
            }
        }
    }

    fclose(f);
    return proj;
}

/* ── iron_toml_free ──────────────────────────────────────────────────────── */

void iron_toml_free(IronProject *proj) {
    if (!proj) return;
    free(proj->name);
    free(proj->version);
    free(proj->entry);
    free(proj);
}
