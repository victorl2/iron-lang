#ifndef IRON_PKG_FETCHER_H
#define IRON_PKG_FETCHER_H

#include <stdbool.h>
#include <stddef.h>

/* Returns malloc'd GitHub auth token from IRON_GITHUB_TOKEN or GITHUB_TOKEN env.
 * Returns NULL if neither is set. Caller must free. */
char *get_github_token(void);

/* Build the cache base directory path: ~/.iron/cache
 * Returns 0 on success, -1 on failure. Creates directory if needed. */
int get_cache_base(char *buf, size_t buf_size);

/* Build full cache path for a dep: ~/.iron/cache/{owner}/{repo}@{sha}/
 * owner and repo are split from the "owner/repo" git field. */
void dep_cache_path(char *buf, size_t buf_size,
                    const char *git, const char *sha);

/* Resolve a version tag to a 40-char commit SHA via GitHub API.
 * Tries "v{version}" first, then "{version}".
 * Handles annotated tags (two-step dereference).
 * token may be NULL for unauthenticated requests.
 * Returns malloc'd 40-char SHA string on success, NULL on failure.
 * Prints error messages to stderr on failure. */
char *resolve_tag_to_sha(const char *git, const char *version,
                         const char *token, bool colors);

/* Download and extract dependency source into the cache.
 * git = "owner/repo", sha = 40-char commit SHA.
 * cache_dir = output from dep_cache_path.
 * Skips download if cache_dir already exists.
 * Returns 0 on success, non-zero on failure. */
int fetch_dependency(const char *name, const char *version, const char *git,
                     const char *sha, const char *cache_dir,
                     const char *token, bool colors);

#endif /* IRON_PKG_FETCHER_H */
