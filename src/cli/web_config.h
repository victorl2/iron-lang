#ifndef IRON_CLI_WEB_CONFIG_H
#define IRON_CLI_WEB_CONFIG_H

#include <stdbool.h>

/* Default values for [web] numeric fields.
 * Used when the field is absent from iron.toml (represented as 0 on the struct). */
#define IRON_WEB_DEFAULT_INITIAL_MEMORY 134217728   /* 128 MiB */
#define IRON_WEB_DEFAULT_STACK_SIZE 1048576          /* 1 MiB  */
#define IRON_WEB_DEFAULT_PTHREAD_POOL_SIZE 4

/* Parsed representation of the [web] section in iron.toml.
 * Embedded by value on IronProject.  Zero-value == "not set" for all fields:
 *   - pointer fields NULL means not set
 *   - int fields 0 means not set (use IRON_WEB_DEFAULT_* instead)
 */
typedef struct {
    char  **assets;             /* array of preload paths (WEB-MANIFEST-02). NULL if not set. */
    int     asset_count;        /* number of entries in assets[]; 0 if not set. */
    char   *title;              /* [web].title for HTML <title>. NULL if not set. */
    char   *shell;              /* [web].shell path to custom shell template. NULL = use default. */
    int     initial_memory;     /* [web].initial_memory (bytes). 0 means use IRON_WEB_DEFAULT_INITIAL_MEMORY. */
    int     stack_size;         /* [web].stack_size (bytes). 0 means use IRON_WEB_DEFAULT_STACK_SIZE. */
    int     pthread_pool_size;  /* [web].pthread_pool_size. 0 means use IRON_WEB_DEFAULT_PTHREAD_POOL_SIZE. */
} IronWebConfig;

#endif /* IRON_CLI_WEB_CONFIG_H */
