#include "cli/toml.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ── Minimal iron.toml parser ────────────────────────────────────────────── */
/*
 * Supports only the fields used by Iron:
 *   [package]    (or [project] for backward compat)
 *     name        = "my_game"
 *     version     = "0.1.0"
 *     entry       = "src/main.iron"  (kept for compat, ignored by iron)
 *     type        = "bin"            (or "lib")
 *     description = "A cool project"
 *   [dependencies]
 *     raylib    = true
 *     iron-ecs  = { git = "owner/iron-ecs", version = "0.2.0" }
 *
 * Comments: # to end of line.
 * String values: "quoted" or bare words.
 * Inline tables: { key = "value", key2 = "value2" }
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

/* ── Helper: detect inline table value (starts with '{') ─────────────────── */

static bool is_inline_table(const char *val) {
    while (*val && isspace((unsigned char)*val)) val++;
    return *val == '{';
}

/* ── Helper: extract a named field from an inline table string ───────────── */
/*
 * Given a val like: { git = "owner/repo", version = "0.2.0" }
 * and a field name like "git", returns a malloc'd copy of "owner/repo".
 * Returns NULL if the field is not found or not a quoted string.
 * Handles quoted strings containing commas/braces correctly.
 */
static char *extract_inline_field(const char *val, const char *field) {
    size_t field_len = strlen(field);
    const char *p = val;

    /* Skip leading '{' */
    while (*p && *p != '{') p++;
    if (!*p) return NULL;
    p++; /* skip '{' */

    while (*p) {
        /* Skip whitespace */
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p || *p == '}') break;

        /* Read key */
        const char *key_start = p;
        while (*p && *p != '=' && *p != '}' && !isspace((unsigned char)*p)) p++;
        size_t key_len = (size_t)(p - key_start);

        /* Skip whitespace before '=' */
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p != '=') {
            /* Malformed, skip to next comma */
            while (*p && *p != ',' && *p != '}') p++;
            if (*p == ',') p++;
            continue;
        }
        p++; /* skip '=' */

        /* Skip whitespace before value */
        while (*p && isspace((unsigned char)*p)) p++;

        if (*p == '"') {
            /* Quoted string value */
            p++; /* skip opening '"' */
            const char *val_start = p;
            while (*p && *p != '"') p++;
            size_t val_len = (size_t)(p - val_start);
            if (*p == '"') p++; /* skip closing '"' */

            /* Is this the field we want? */
            if (key_len == field_len && strncmp(key_start, field, field_len) == 0) {
                char *result = (char *)malloc(val_len + 1);
                if (!result) return NULL;
                memcpy(result, val_start, val_len);
                result[val_len] = '\0';
                return result;
            }
        } else {
            /* Bare word value */
            const char *val_start = p;
            while (*p && *p != ',' && *p != '}' && !isspace((unsigned char)*p)) p++;
            if (key_len == field_len && strncmp(key_start, field, field_len) == 0) {
                size_t val_len = (size_t)(p - val_start);
                char *result = (char *)malloc(val_len + 1);
                if (!result) return NULL;
                memcpy(result, val_start, val_len);
                result[val_len] = '\0';
                return result;
            }
        }

        /* Skip to next field */
        while (*p && *p != ',' && *p != '}') p++;
        if (*p == ',') p++;
    }

    return NULL;
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

    /* Track current section: 0 = none, 1 = [package]/[project], 2 = [dependencies] */
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

        /* Section header: [package], [project], or [dependencies] */
        if (*s == '[') {
            char *end = strchr(s, ']');
            if (end) {
                *end = '\0';
                char *sec_name = trim(s + 1);
                if (strcmp(sec_name, "package") == 0 || strcmp(sec_name, "project") == 0) {
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
            /* [package]/[project] section */
            if (strcmp(key, "name") == 0) {
                free(proj->name);
                proj->name = extract_value(val_str);
            } else if (strcmp(key, "version") == 0) {
                free(proj->version);
                proj->version = extract_value(val_str);
            } else if (strcmp(key, "entry") == 0) {
                free(proj->entry);
                proj->entry = extract_value(val_str);
            } else if (strcmp(key, "type") == 0) {
                free(proj->type);
                proj->type = extract_value(val_str);
            } else if (strcmp(key, "description") == 0) {
                free(proj->description);
                proj->description = extract_value(val_str);
            }
        } else if (section == 2) {
            /* [dependencies] section */
            if (strcmp(key, "raylib") == 0 && strcmp(val_str, "true") == 0) {
                proj->raylib = true;
            } else if (is_inline_table(val_str)) {
                /* Grow deps array if needed */
                if (proj->dep_count >= proj->dep_capacity) {
                    int new_cap = proj->dep_capacity == 0 ? 4 : proj->dep_capacity * 2;
                    IronDep *new_deps = (IronDep *)realloc(proj->deps,
                                                           sizeof(IronDep) * (size_t)new_cap);
                    if (!new_deps) continue;
                    proj->deps = new_deps;
                    proj->dep_capacity = new_cap;
                }
                IronDep *dep = &proj->deps[proj->dep_count];
                memset(dep, 0, sizeof(IronDep));
                dep->name    = strdup(key);
                dep->git     = extract_inline_field(val_str, "git");
                dep->version = extract_inline_field(val_str, "version");
                proj->dep_count++;
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
    free(proj->type);
    free(proj->description);
    for (int i = 0; i < proj->dep_count; i++) {
        free(proj->deps[i].name);
        free(proj->deps[i].git);
        free(proj->deps[i].version);
    }
    free(proj->deps);
    free(proj);
}
