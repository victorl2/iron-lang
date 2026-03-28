/*
 * iron dependency fetcher: GitHub API tag resolution, tarball download, cache management.
 *
 * Resolves version tags to 40-char commit SHAs via the GitHub REST API,
 * downloads source tarballs, and extracts them into ~/.iron/cache/.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include <stdbool.h>
#include <ctype.h>

#ifdef _WIN32
#  include <windows.h>
#  include <direct.h>
#else
#  include <unistd.h>
#  include <dirent.h>
#endif

#include "pkg/iron_pkg.h"
#include "pkg/color.h"
#include "pkg/fetcher.h"

/* ── Helpers ───────────────────────────────────────────────────────────── */

/* Recursive mkdir: create all components along the path. */
static int iron_mkdirp(const char *path) {
    char tmp[2048];
    size_t len = strlen(path);
    if (len >= sizeof(tmp)) return -1;
    memcpy(tmp, path, len + 1);

    for (size_t i = 1; i < len; i++) {
        if (tmp[i] == '/' || tmp[i] == '\\') {
            tmp[i] = '\0';
#ifdef _WIN32
            _mkdir(tmp);
#else
            mkdir(tmp, 0755);
#endif
            tmp[i] = '/';
        }
    }
#ifdef _WIN32
    _mkdir(tmp);
#else
    mkdir(tmp, 0755);
#endif
    return 0;
}

/* Split "owner/repo" into separate strings. Returns 0 on success. */
static int split_git_field(const char *git, char *owner, size_t owner_sz,
                           char *repo, size_t repo_sz) {
    const char *slash = strchr(git, '/');
    if (!slash) return -1;

    size_t olen = (size_t)(slash - git);
    size_t rlen = strlen(slash + 1);
    if (olen >= owner_sz || rlen >= repo_sz) return -1;

    memcpy(owner, git, olen);
    owner[olen] = '\0';
    memcpy(repo, slash + 1, rlen);
    repo[rlen] = '\0';
    return 0;
}

/*
 * Minimal JSON field extractor.
 * Given JSON text and a field name, returns malloc'd string value.
 * Only handles string values (not numbers/bools/objects).
 * Returns NULL if field not found or not a string.
 */
static char *json_extract(const char *json, const char *field) {
    /* Build pattern: "field" */
    char pattern[256];
    snprintf(pattern, sizeof(pattern), "\"%s\"", field);

    const char *pos = strstr(json, pattern);
    if (!pos) return NULL;

    /* Skip past "field" */
    pos += strlen(pattern);

    /* Skip whitespace and colon */
    while (*pos && (*pos == ' ' || *pos == '\t' || *pos == '\n' || *pos == '\r'))
        pos++;
    if (*pos != ':') return NULL;
    pos++;
    while (*pos && (*pos == ' ' || *pos == '\t' || *pos == '\n' || *pos == '\r'))
        pos++;

    if (*pos != '"') return NULL;
    pos++; /* skip opening quote */

    const char *end = strchr(pos, '"');
    if (!end) return NULL;

    size_t len = (size_t)(end - pos);
    char *result = (char *)malloc(len + 1);
    if (!result) return NULL;
    memcpy(result, pos, len);
    result[len] = '\0';
    return result;
}

/* Run curl and write output to a file. Returns exit code. */
static int run_curl_to_file(const char *url, const char *token,
                            const char *outpath) {
    /* Build argv: curl -sL [-H auth] -H user-agent -o outpath url */
    char *argv[16];
    int argc = 0;
    argv[argc++] = (char *)"curl";
    argv[argc++] = (char *)"-sL";

    char auth_header[512];
    if (token) {
        snprintf(auth_header, sizeof(auth_header),
                 "Authorization: Bearer %s", token);
        argv[argc++] = (char *)"-H";
        argv[argc++] = auth_header;
    }

    argv[argc++] = (char *)"-H";
    argv[argc++] = (char *)"User-Agent: iron/0.0.3";
    argv[argc++] = (char *)"-H";
    argv[argc++] = (char *)"Accept: application/vnd.github+json";
    argv[argc++] = (char *)"-o";
    argv[argc++] = (char *)outpath;
    argv[argc++] = (char *)url;
    argv[argc] = NULL;

    return spawn_and_wait("curl", (char *const *)argv);
}

/* Read a file into a buffer. Returns bytes read, or -1 on error. */
static long read_file_to_buf(const char *path, char *buf, size_t buf_size) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    long n = (long)fread(buf, 1, buf_size - 1, f);
    fclose(f);
    if (n >= 0) buf[n] = '\0';
    return n;
}

/*
 * Fetch the SHA for a specific tag via GitHub API.
 * Handles both lightweight tags (type=commit) and annotated tags (type=tag).
 * For annotated tags, makes a second API call to dereference to the commit SHA.
 * Returns malloc'd 40-char SHA on success, NULL on failure.
 */
static char *fetch_ref_sha(const char *owner, const char *repo,
                           const char *tag, const char *token) {
    char url[1024];
    snprintf(url, sizeof(url),
             "https://api.github.com/repos/%s/%s/git/refs/tags/%s",
             owner, repo, tag);

    char tmp_path[512];
#ifdef _WIN32
    const char *tmp_base = getenv("TEMP");
    if (!tmp_base) tmp_base = "C:\\Temp";
    snprintf(tmp_path, sizeof(tmp_path), "%s\\iron_tag_%s_%s.json",
             tmp_base, owner, repo);
#else
    snprintf(tmp_path, sizeof(tmp_path), "/tmp/iron_tag_%s_%s.json",
             owner, repo);
#endif

    int ret = run_curl_to_file(url, token, tmp_path);
    if (ret != 0) {
        remove(tmp_path);
        return NULL;
    }

    char buf[8192];
    long n = read_file_to_buf(tmp_path, buf, sizeof(buf));
    if (n <= 0) {
        remove(tmp_path);
        return NULL;
    }

    /* Check for 404 / error responses */
    if (strstr(buf, "\"Not Found\"") || strstr(buf, "\"not found\"")) {
        remove(tmp_path);
        return NULL;
    }

    /* The response has nested "object" with "type" and "sha" */
    char *type = json_extract(buf, "type");
    char *sha = json_extract(buf, "sha");

    if (!type || !sha) {
        free(type);
        free(sha);
        remove(tmp_path);
        return NULL;
    }

    /* For annotated tags, the sha is the tag object, not the commit.
     * Need a second call to dereference. */
    if (strcmp(type, "tag") == 0) {
        char url2[1024];
        snprintf(url2, sizeof(url2),
                 "https://api.github.com/repos/%s/%s/git/tags/%s",
                 owner, repo, sha);
        free(sha);
        sha = NULL;

        ret = run_curl_to_file(url2, token, tmp_path);
        if (ret != 0) {
            free(type);
            remove(tmp_path);
            return NULL;
        }

        n = read_file_to_buf(tmp_path, buf, sizeof(buf));
        if (n <= 0) {
            free(type);
            remove(tmp_path);
            return NULL;
        }

        /* The tag object response has "object" -> "sha" which is the commit */
        sha = json_extract(buf, "sha");
    }

    free(type);
    remove(tmp_path);

    /* Validate SHA is 40 hex chars */
    if (sha && strlen(sha) >= 40) {
        sha[40] = '\0'; /* truncate to 40 chars if longer */
        return sha;
    }

    free(sha);
    return NULL;
}

/* ── Public API ────────────────────────────────────────────────────────── */

char *get_github_token(void) {
    const char *tok = getenv("IRON_GITHUB_TOKEN");
    if (tok && tok[0]) return strdup(tok);
    tok = getenv("GITHUB_TOKEN");
    if (tok && tok[0]) return strdup(tok);
    return NULL;
}

int get_cache_base(char *buf, size_t buf_size) {
#ifdef _WIN32
    const char *home = getenv("USERPROFILE");
#else
    const char *home = getenv("HOME");
#endif
    if (!home || !home[0]) return -1;

    snprintf(buf, buf_size, "%s/.iron/cache", home);
    iron_mkdirp(buf);
    return 0;
}

void dep_cache_path(char *buf, size_t buf_size,
                    const char *git, const char *sha) {
    char base[2048];
    if (get_cache_base(base, sizeof(base)) != 0) {
        snprintf(buf, buf_size, ".iron-cache/%s@%s", git, sha);
        return;
    }

    char owner[256], repo[256];
    if (split_git_field(git, owner, sizeof(owner), repo, sizeof(repo)) != 0) {
        snprintf(buf, buf_size, "%s/%s@%s", base, git, sha);
        return;
    }

    snprintf(buf, buf_size, "%s/%s/%s@%s", base, owner, repo, sha);
}

char *resolve_tag_to_sha(const char *git, const char *version,
                         const char *token, bool colors) {
    char owner[256], repo[256];
    if (split_git_field(git, owner, sizeof(owner), repo, sizeof(repo)) != 0) {
        char msg[512];
        snprintf(msg, sizeof(msg),
                 "invalid git field '%s': expected 'owner/repo' format", git);
        iron_print_error(colors, msg);
        return NULL;
    }

    /* Try v{version} first */
    char vtag[128];
    snprintf(vtag, sizeof(vtag), "v%s", version);
    char *sha = fetch_ref_sha(owner, repo, vtag, token);
    if (sha) return sha;

    /* Try {version} without v prefix */
    sha = fetch_ref_sha(owner, repo, version, token);
    if (sha) return sha;

    /* Check for rate limiting */
    char tmp_path[512];
#ifdef _WIN32
    const char *tmp_base = getenv("TEMP");
    if (!tmp_base) tmp_base = "C:\\Temp";
    snprintf(tmp_path, sizeof(tmp_path), "%s\\iron_tag_%s_%s.json",
             tmp_base, owner, repo);
#else
    snprintf(tmp_path, sizeof(tmp_path), "/tmp/iron_tag_%s_%s.json",
             owner, repo);
#endif

    char buf[4096];
    long n = read_file_to_buf(tmp_path, buf, sizeof(buf));
    if (n > 0) {
        /* Case-insensitive search for rate limit */
        char lower[4096];
        size_t blen = strlen(buf);
        if (blen >= sizeof(lower)) blen = sizeof(lower) - 1;
        for (size_t i = 0; i < blen; i++)
            lower[i] = (char)tolower((unsigned char)buf[i]);
        lower[blen] = '\0';

        if (strstr(lower, "rate limit")) {
            iron_print_error(colors, "GitHub API rate limit exceeded");
            fprintf(stderr,
                    "  hint: set IRON_GITHUB_TOKEN for higher rate limits "
                    "(5000/hr vs 60/hr)\n");
            return NULL;
        }
    }

    /* Tag not found */
    char msg[512];
    snprintf(msg, sizeof(msg),
             "tag '%s' not found for %s", version, git);
    iron_print_error(colors, msg);
    fprintf(stderr,
            "  hint: check that the repo and version tag exist on GitHub\n");
    return NULL;
}

int fetch_dependency(const char *name, const char *version, const char *git,
                     const char *sha, const char *cache_dir,
                     const char *token, bool colors) {
    /* Cache hit: directory already exists */
    struct stat st;
    if (stat(cache_dir, &st) == 0) {
        return 0; /* already cached */
    }

    /* Print download status */
    char detail[512];
    snprintf(detail, sizeof(detail), "%s v%s (%s)", name, version, git);
    iron_print_status(colors, "Downloading", detail);

    char owner[256], repo[256];
    if (split_git_field(git, owner, sizeof(owner), repo, sizeof(repo)) != 0) {
        iron_print_error(colors, "invalid git field for dependency");
        return 1;
    }

    /* Build tarball URL */
    char url[1024];
    snprintf(url, sizeof(url),
             "https://github.com/%s/%s/archive/%s.tar.gz",
             owner, repo, sha);

    /* Temp file for tarball */
    char tmp_tar[512];
#ifdef _WIN32
    const char *tmp_base = getenv("TEMP");
    if (!tmp_base) tmp_base = "C:\\Temp";
    snprintf(tmp_tar, sizeof(tmp_tar), "%s\\iron_dep_%s.tar.gz",
             tmp_base, name);
#else
    snprintf(tmp_tar, sizeof(tmp_tar), "/tmp/iron_dep_%s.tar.gz", name);
#endif

    /* Download tarball */
    int ret = run_curl_to_file(url, token, tmp_tar);
    if (ret != 0) {
        char msg[1024];
        snprintf(msg, sizeof(msg),
                 "failed to download %s from %s", name, url);
        iron_print_error(colors, msg);
        remove(tmp_tar);
        return 1;
    }

    /* Create temp extraction directory */
    char tmp_extract[2048];
    char base[2048];
    get_cache_base(base, sizeof(base));
    snprintf(tmp_extract, sizeof(tmp_extract),
             "%s/%s/_iron_tmp_%s_%s", base, owner, repo, sha);
    iron_mkdirp(tmp_extract);

    /* Extract tarball */
    char *tar_argv[] = {
        (char *)"tar", (char *)"xf", tmp_tar,
        (char *)"-C", tmp_extract, NULL
    };
    ret = spawn_and_wait("tar", (char *const *)tar_argv);
    remove(tmp_tar);

    if (ret != 0) {
        char msg[512];
        snprintf(msg, sizeof(msg),
                 "failed to extract tarball for %s", name);
        iron_print_error(colors, msg);
        return 1;
    }

    /* Find the single subdirectory inside tmp_extract
     * (GitHub names it {repo}-{sha_prefix}/) */
    char found_subdir[2048];
    found_subdir[0] = '\0';

#ifdef _WIN32
    char search_pattern[2048];
    snprintf(search_pattern, sizeof(search_pattern), "%s\\*", tmp_extract);
    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA(search_pattern, &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (strcmp(fd.cFileName, ".") != 0 &&
                    strcmp(fd.cFileName, "..") != 0) {
                    snprintf(found_subdir, sizeof(found_subdir),
                             "%s\\%s", tmp_extract, fd.cFileName);
                    break;
                }
            }
        } while (FindNextFileA(hFind, &fd));
        FindClose(hFind);
    }
#else
    DIR *d = opendir(tmp_extract);
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (ent->d_name[0] == '.') continue;
            snprintf(found_subdir, sizeof(found_subdir),
                     "%s/%s", tmp_extract, ent->d_name);
            break;
        }
        closedir(d);
    }
#endif

    if (found_subdir[0] == '\0') {
        char msg[512];
        snprintf(msg, sizeof(msg),
                 "no directory found in extracted tarball for %s", name);
        iron_print_error(colors, msg);
        return 1;
    }

    /* Ensure parent of cache_dir exists */
    char cache_parent[2048];
    snprintf(cache_parent, sizeof(cache_parent), "%s/%s", base, owner);
    iron_mkdirp(cache_parent);

    /* Rename extracted dir to final cache location */
    ret = rename(found_subdir, cache_dir);
    if (ret != 0) {
        char msg[512];
        snprintf(msg, sizeof(msg),
                 "failed to move extracted source to cache: %s",
                 strerror(errno));
        iron_print_error(colors, msg);
        return 1;
    }

    /* Clean up temp extraction dir */
    rmdir(tmp_extract);

    /* Verify final location exists */
    if (stat(cache_dir, &st) != 0) {
        char msg[512];
        snprintf(msg, sizeof(msg),
                 "cache directory missing after extraction: %s", cache_dir);
        iron_print_error(colors, msg);
        return 1;
    }

    return 0;
}
