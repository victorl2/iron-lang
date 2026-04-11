#include "cli/toml.h"

#include <limits.h>
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

/* ── Helper: Levenshtein distance for misspelled-section detection ──────── */
/*
 * Naive single-row rolling DP.  Input strings are bounded by line length
 * (< 1024) so a 66-element stack array is sufficient given the 64-char cap.
 * Returns INT_MAX if either string exceeds 64 characters (unreasonable for
 * section names).  Characters are compared case-sensitively; TOML section
 * headers are case-sensitive ([Web] ≠ [web]).
 */
static int levenshtein(const char *a, const char *b) {
    size_t la = strlen(a);
    size_t lb = strlen(b);
    if (la > 64 || lb > 64) return INT_MAX;

    /* v0[j] = edit distance between a[0..i-1] and b[0..j-1] */
    int v0[66];
    int v1[66];

    /* Initialize: distance from empty string to b[0..j] */
    for (size_t j = 0; j <= lb; j++) v0[j] = (int)j;

    for (size_t i = 0; i < la; i++) {
        v1[0] = (int)(i + 1);
        for (size_t j = 0; j < lb; j++) {
            int del_cost  = v0[j + 1] + 1;
            int ins_cost  = v1[j]     + 1;
            int sub_cost  = v0[j] + (a[i] != b[j] ? 1 : 0);
            int best = del_cost < ins_cost ? del_cost : ins_cost;
            v1[j + 1] = best < sub_cost ? best : sub_cost;
        }
        /* swap rows */
        for (size_t j = 0; j <= lb; j++) v0[j] = v1[j];
    }
    return v0[lb];
}

/* ── Known section names (for Levenshtein typo detection) ───────────────── */
static const char *KNOWN_SECTIONS[] = {
    "package",
    "project",
    "dependencies",
    "web",
    NULL
};

/* ── Helper: parse TOML inline string array into char** + count ─────────── */
/*
 * Given a string starting with '[', parses comma-separated quoted strings.
 * Example: ["a.png", "b.png"]
 * Fills *out_arr (malloc'd array of strdup'd strings) and *out_count.
 * Returns 0 on success, non-zero on malformed input (frees partial results).
 * The helper is kept static; do NOT expose in any header.
 */
static int parse_toml_string_array(const char *val, char ***out_arr, int *out_count) {
    const char *p = val;
    /* skip leading whitespace and '[' */
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '[') return 1;
    p++; /* skip '[' */

    char **arr = NULL;
    int count  = 0;
    int cap    = 0;

    while (1) {
        /* skip whitespace */
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) goto fail; /* unexpected end */
        if (*p == ']') break; /* end of array */

        /* expect opening quote */
        if (*p != '"') goto fail;
        p++; /* skip '"' */
        const char *elem_start = p;
        while (*p && *p != '"') p++;
        if (*p != '"') goto fail; /* unclosed quote */
        size_t elem_len = (size_t)(p - elem_start);
        p++; /* skip closing '"' */

        /* grow array if needed */
        if (count >= cap) {
            int new_cap = cap == 0 ? 4 : cap * 2;
            char **new_arr = (char **)realloc(arr, sizeof(char *) * (size_t)new_cap);
            if (!new_arr) goto fail;
            arr = new_arr;
            cap = new_cap;
        }
        char *elem = (char *)malloc(elem_len + 1);
        if (!elem) goto fail;
        memcpy(elem, elem_start, elem_len);
        elem[elem_len] = '\0';
        arr[count++] = elem;

        /* skip whitespace then optional ',' */
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == ',') p++;
    }

    *out_arr   = arr;
    *out_count = count;
    return 0;

fail:
    for (int fi = 0; fi < count; fi++) free(arr[fi]);
    free(arr);
    *out_arr   = NULL;
    *out_count = 0;
    return 1;
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

        /* Section header: [package], [project], [dependencies], or [web] */
        if (*s == '[') {
            char *end = strchr(s, ']');
            if (end) {
                *end = '\0';
                char *sec_name = trim(s + 1);
                if (strcmp(sec_name, "package") == 0 || strcmp(sec_name, "project") == 0) {
                    section = 1;
                } else if (strcmp(sec_name, "dependencies") == 0) {
                    section = 2;
                } else if (strcmp(sec_name, "web") == 0) {
                    section = 3;
                } else {
                    /* Unknown section: check if it's a typo of a known one (Levenshtein ≤2). */
                    int best_dist = INT_MAX;
                    const char *best_match = NULL;
                    for (int ki = 0; KNOWN_SECTIONS[ki]; ki++) {
                        int d = levenshtein(sec_name, KNOWN_SECTIONS[ki]);
                        if (d < best_dist) {
                            best_dist = d;
                            best_match = KNOWN_SECTIONS[ki];
                        }
                    }
                    if (best_dist > 0 && best_dist <= 2 && best_match) {
                        fprintf(stderr, "warning: unknown section [%s] — did you mean [%s]? (ignored)\n",
                                sec_name, best_match);
                    }
                    /* distance > 2 or no match: silently ignore. */
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
        } else if (section == 3) {
            /* [web] section (Phase 2 — WEB-MANIFEST-01..08) */
            if (strcmp(key, "title") == 0) {
                free(proj->web.title);
                proj->web.title = extract_value(val_str);
            } else if (strcmp(key, "shell") == 0) {
                free(proj->web.shell);
                proj->web.shell = extract_value(val_str);
            } else if (strcmp(key, "initial_memory") == 0) {
                char *endp = NULL;
                long v = strtol(val_str, &endp, 10);
                if (endp == val_str || v <= 0) {
                    fprintf(stderr, "warning: [web].initial_memory must be a positive integer (got '%s', ignored)\n", val_str);
                } else {
                    proj->web.initial_memory = (int)v;
                }
            } else if (strcmp(key, "stack_size") == 0) {
                char *endp = NULL;
                long v = strtol(val_str, &endp, 10);
                if (endp == val_str || v <= 0) {
                    fprintf(stderr, "warning: [web].stack_size must be a positive integer (got '%s', ignored)\n", val_str);
                } else {
                    proj->web.stack_size = (int)v;
                }
            } else if (strcmp(key, "pthread_pool_size") == 0) {
                char *endp = NULL;
                long v = strtol(val_str, &endp, 10);
                if (endp == val_str || v <= 0) {
                    fprintf(stderr, "warning: [web].pthread_pool_size must be a positive integer (got '%s', ignored)\n", val_str);
                } else {
                    proj->web.pthread_pool_size = (int)v;
                }
            } else if (strcmp(key, "assets") == 0) {
                /* Free previously-set assets (last-write-wins) */
                for (int ai = 0; ai < proj->web.asset_count; ai++) free(proj->web.assets[ai]);
                free(proj->web.assets);
                proj->web.assets = NULL;
                proj->web.asset_count = 0;
                /* Determine array vs scalar form */
                const char *trimmed = val_str;
                while (*trimmed && isspace((unsigned char)*trimmed)) trimmed++;
                if (*trimmed == '[') {
                    if (parse_toml_string_array(trimmed, &proj->web.assets, &proj->web.asset_count) != 0) {
                        fprintf(stderr, "warning: [web].assets malformed (ignored)\n");
                        proj->web.assets = NULL;
                        proj->web.asset_count = 0;
                    }
                } else {
                    /* Scalar string form — normalize to one-element array */
                    char *one = extract_value(val_str);
                    if (one) {
                        proj->web.assets = (char **)malloc(sizeof(char *));
                        if (proj->web.assets) {
                            proj->web.assets[0] = one;
                            proj->web.asset_count = 1;
                        } else {
                            free(one);
                        }
                    }
                }
            } else {
                fprintf(stderr, "warning: unknown [web] key '%s' (ignored)\n", key);
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
        free(proj->deps[i].sha);
        free(proj->deps[i].cache_path);
    }
    free(proj->deps);
    free(proj);
}
