#ifndef IRON_CLI_TOML_H
#define IRON_CLI_TOML_H

#include <stdbool.h>

/* Parsed representation of an iron.toml project file. */
typedef struct {
    char *name;      /* [project] name = "..." */
    char *version;   /* [project] version = "..." */
    char *entry;     /* [project] entry = "..." */
    bool  raylib;    /* [dependencies] raylib = true */
} IronProject;

/* Parse iron.toml at the given path.
 * Returns a heap-allocated IronProject on success, NULL on error.
 * Caller must free with iron_toml_free(). */
IronProject *iron_toml_parse(const char *path);

/* Free an IronProject returned by iron_toml_parse(). */
void iron_toml_free(IronProject *proj);

#endif /* IRON_CLI_TOML_H */
