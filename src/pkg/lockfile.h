#ifndef IRON_PKG_LOCKFILE_H
#define IRON_PKG_LOCKFILE_H

/* Parsed representation of a single [[package]] entry in iron.lock */
typedef struct {
    char *name;      /* name = "iron-ecs" */
    char *version;   /* version = "0.1.0" */
    char *git;       /* git = "owner/repo" */
    char *sha;       /* sha = "a1b2c3...40chars" */
} IronLockEntry;

/* Read iron.lock at the given path.
 * Populates *out_entries with a malloc'd array of IronLockEntry.
 * Returns count of entries read (0 if file doesn't exist or is empty).
 * Returns -1 on parse error. */
int lockfile_read(const char *path, IronLockEntry **out_entries);

/* Write iron.lock to the given path.
 * Sorts entries alphabetically by name before writing.
 * Writes lock_version = 1 header and [[package]] entries. */
void lockfile_write(const char *path, IronLockEntry *entries, int count);

/* Free an array of IronLockEntry returned by lockfile_read(). */
void lockfile_free(IronLockEntry *entries, int count);

/* Look up a dependency by git field (e.g. "owner/repo") in lock entries.
 * Comparison is case-insensitive (GitHub repos are case-insensitive).
 * Returns pointer to the matching entry, or NULL if not found.
 * The returned pointer is into the entries array (do not free individually). */
IronLockEntry *lockfile_find(IronLockEntry *entries, int count, const char *git);

#endif /* IRON_PKG_LOCKFILE_H */
