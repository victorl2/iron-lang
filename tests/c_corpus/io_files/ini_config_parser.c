/*
 * title: INI style config file parser
 * topic: io_files
 * covers: fgets line parsing, sections, comments, whitespace trimming, quoted values, line continuation, error reporting with line numbers
 * deps: libc
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { MAXE = 32 };

typedef struct {
    char section[24];
    char key[24];
    char val[80];
} Entry;

static Entry entries[MAXE];
static int nent = 0;

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

static char *trim(char *s) {
    while (isspace((unsigned char)*s))
        s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1]))
        *--e = 0;
    return s;
}

static int parse_ini(FILE *f, int *errors) {
    char section[24] = "";
    char line[160];
    char pending[160];
    int have_pending = 0, lineno = 0;
    *errors = 0;
    while (fgets(line, sizeof line, f)) {
        lineno++;
        char *s = trim(line);
        char joined[320];
        if (have_pending) {
            snprintf(joined, sizeof joined, "%s%s", pending, s);
            s = joined;
            have_pending = 0;
        }
        size_t n = strlen(s);
        if (n > 0 && s[n - 1] == '\\') {
            s[n - 1] = 0;
            snprintf(pending, sizeof pending, "%s", s);
            have_pending = 1;
            continue;
        }
        if (*s == 0 || *s == '#' || *s == ';')
            continue;
        if (*s == '[') {
            char *close = strchr(s, ']');
            if (!close || close == s + 1) {
                printf("line %d: bad section header\n", lineno);
                (*errors)++;
                continue;
            }
            *close = 0;
            snprintf(section, sizeof section, "%s", trim(s + 1));
            continue;
        }
        char *eq = strchr(s, '=');
        if (!eq) {
            printf("line %d: expected key=value\n", lineno);
            (*errors)++;
            continue;
        }
        *eq = 0;
        char *key = trim(s), *val = trim(eq + 1);
        if (*key == 0) {
            printf("line %d: empty key\n", lineno);
            (*errors)++;
            continue;
        }
        size_t vl = strlen(val);
        if (vl >= 2 && val[0] == '"' && val[vl - 1] == '"') {
            val[vl - 1] = 0;
            val++;
        } else {
            char *hash = strstr(val, " #");
            if (hash) {
                *hash = 0;
                val = trim(val);
            }
        }
        check(nent < MAXE, "too many entries");
        snprintf(entries[nent].section, sizeof entries[nent].section, "%s", section);
        snprintf(entries[nent].key, sizeof entries[nent].key, "%s", key);
        snprintf(entries[nent].val, sizeof entries[nent].val, "%s", val);
        nent++;
    }
    return nent;
}

static const char *get(const char *sec, const char *key, const char *def) {
    for (int i = nent - 1; i >= 0; i--) /* last definition wins */
        if (strcmp(entries[i].section, sec) == 0 && strcmp(entries[i].key, key) == 0)
            return entries[i].val;
    return def;
}

int main(void) {
    const char *ini =
        "# global settings\n"
        "name = iron-reference   # trailing comment\n"
        "verbose=true\n"
        "\n"
        "[server]\n"
        "host = 127.0.0.1\n"
        "port = 8080\n"
        "banner = \"  hello ; world  \"\n"
        "path = /usr/local/\\\n"
        "  share/data\n"
        "\n"
        "; a comment\n"
        "[client]\n"
        "retries = 5\n"
        "just a bare line\n"
        "= novalue\n"
        "timeout = 2.5\n"
        "[]\n"
        "[server]\n"
        "port = 9090\n";
    FILE *f = fopen("app.ini", "w");
    check(f != NULL, "open");
    fputs(ini, f);
    fclose(f);

    f = fopen("app.ini", "r");
    int errors;
    int n = parse_ini(f, &errors);
    fclose(f);
    printf("%d entries, %d errors\n", n, errors);
    check(n == 9 && errors == 3, "counts");
    for (int i = 0; i < n; i++)
        printf("  [%s] %s = \"%s\"\n", entries[i].section, entries[i].key, entries[i].val);

    printf("server.port (last wins) = %s\n", get("server", "port", "?"));
    printf("server.path = %s\n", get("server", "path", "?"));
    printf("client.retries + 1 = %d\n", atoi(get("client", "retries", "0")) + 1);
    printf("client.timeout * 2 = %.1f\n", atof(get("client", "timeout", "0")) * 2);
    printf("missing.key default = %s\n", get("missing", "key", "(default)"));
    check(strcmp(get("", "name", ""), "iron-reference") == 0, "global name");
    check(strcmp(get("server", "banner", ""), "  hello ; world  ") == 0, "quoted");
    check(strcmp(get("server", "path", ""), "/usr/local/share/data") == 0, "continuation");
    remove("app.ini");
    return 0;
}
