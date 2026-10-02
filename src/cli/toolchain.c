/* toolchain.c: locate, verify and install the pinned C toolchain. See
 * toolchain.h for the lookup order and the rules. */
#include "cli/toolchain.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "lsp/store/sha256.h"
#include "util/os.h"

#ifdef _WIN32
  #include <process.h>
#else
  #include <spawn.h>
  #include <sys/wait.h>
  #ifdef __APPLE__
    #include <mach-o/dyld.h>
  #endif
  extern char **environ;
#endif

#define MANIFEST "toolchain.txt"

/* ── Small helpers ──────────────────────────────────────────────────────── */

static int path_join(char *out, size_t cap, const char *a, const char *b) {
    int n = snprintf(out, cap, "%s/%s", a, b);
    return (n > 0 && (size_t)n < cap) ? 0 : -1;
}

static bool is_dir(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool is_file(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

/* Directory containing this executable. */
static int self_dir(char *buf, size_t cap) {
#ifdef __APPLE__
    uint32_t size = (uint32_t)cap;
    if (_NSGetExecutablePath(buf, &size) != 0) return -1;
#elif defined(__linux__)
    ssize_t n = readlink("/proc/self/exe", buf, cap - 1);
    if (n < 0) return -1;
    buf[n] = '\0';
#elif defined(_WIN32)
    DWORD n = GetModuleFileNameA(NULL, buf, (DWORD)cap);
    if (n == 0 || n >= (DWORD)cap) return -1;
#else
    return -1;
#endif
    char *last = strrchr(buf, '/');
#ifdef _WIN32
    char *lw = strrchr(buf, '\\');
    if (lw > last) last = lw;
#endif
    if (!last) return -1;
    *last = '\0';
    return 0;
}

static const char *home_dir(void) {
    const char *h = getenv("HOME");
#ifdef _WIN32
    if (!h || !*h) h = getenv("USERPROFILE");
#endif
    return (h && *h) ? h : NULL;
}

/* mkdir -p */
static int mkdir_p(const char *path) {
    char buf[4096];
    if (snprintf(buf, sizeof(buf), "%s", path) >= (int)sizeof(buf)) return -1;
    for (char *p = buf + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            char c = *p;
            *p = '\0';
            if (strlen(buf) > 0 && !(buf[1] == ':' && buf[2] == '\0'))
                if (mkdir(buf, 0755) != 0 && errno != EEXIST) return -1;
            *p = c;
        }
    }
    if (mkdir(buf, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
}

/* Run a program found on PATH (curl, tar) and return its exit status, or
 * -1 when it could not be started. */
static int run(char *const argv[]) {
#ifdef _WIN32
    intptr_t rc = _spawnvp(_P_WAIT, argv[0], (const char *const *)argv);
    return rc < 0 ? -1 : (int)rc;
#else
    pid_t pid;
    if (posix_spawnp(&pid, argv[0], NULL, NULL, argv, environ) != 0) return -1;
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

const char *iron_toolchain_sysroot(void) {
#ifdef __APPLE__
    static char path[4096];
    static int state = 0;   /* 0 unresolved, 1 found, -1 none */
    if (state == 0) {
        state = -1;
        const char *env = getenv("SDKROOT");
        if (env && *env && is_dir(env)) {
            snprintf(path, sizeof(path), "%s", env);
            state = 1;
        } else {
            FILE *p = popen("xcrun --show-sdk-path 2>/dev/null", "r");
            if (p) {
                if (fgets(path, sizeof(path), p)) {
                    path[strcspn(path, "\n")] = '\0';
                    if (path[0] && is_dir(path)) state = 1;
                }
                pclose(p);
            }
        }
        if (state < 0)
            fprintf(stderr, "warning: no macOS SDK found (install the Xcode command line tools or set SDKROOT)\n");
    }
    return state == 1 ? path : NULL;
#else
    return NULL;
#endif
}

const char *iron_toolchain_host(void) {
#if defined(__APPLE__) && defined(__aarch64__)
    return "macos-arm64";
#elif defined(__APPLE__)
    return "macos-x86_64";
#elif defined(__linux__) && defined(__aarch64__)
    return "linux-arm64";
#elif defined(__linux__)
    return "linux-x86_64";
#elif defined(_WIN32)
    return "windows-x86_64";
#else
    return "unknown";
#endif
}

static const char *pin_for_host(const char *host) {
    for (size_t i = 0; i < sizeof(IRON_TOOLCHAIN_PINS) / sizeof(IRON_TOOLCHAIN_PINS[0]); i++)
        if (strcmp(IRON_TOOLCHAIN_PINS[i].host, host) == 0) return IRON_TOOLCHAIN_PINS[i].sha256;
    return NULL;
}

/* ── Manifest ───────────────────────────────────────────────────────────── */

/* toolchain.txt:
 *   iron-toolchain 1
 *   llvm 23.1.2
 *   bundle 1
 *   host linux-x86_64
 * Returns 0 and fills tc->llvm/bundle/host, -1 when missing or malformed. */
static int read_manifest(IronToolchain *tc) {
    char path[4096];
    if (path_join(path, sizeof(path), tc->root, MANIFEST) != 0) return -1;
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    tc->llvm[0] = tc->bundle[0] = tc->host[0] = '\0';
    bool header = false;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char key[32], val[128];
        if (sscanf(line, "%31s %127s", key, val) != 2) continue;
        if (strcmp(key, "iron-toolchain") == 0) header = strcmp(val, "1") == 0;
        else if (strcmp(key, "llvm") == 0) snprintf(tc->llvm, sizeof(tc->llvm), "%s", val);
        else if (strcmp(key, "bundle") == 0) snprintf(tc->bundle, sizeof(tc->bundle), "%s", val);
        else if (strcmp(key, "host") == 0) snprintf(tc->host, sizeof(tc->host), "%s", val);
    }
    fclose(f);
    return (header && tc->llvm[0] && tc->bundle[0]) ? 0 : -1;
}

static bool manifest_matches(const IronToolchain *tc) {
    return strcmp(tc->llvm, IRON_TOOLCHAIN_LLVM) == 0 &&
           strcmp(tc->bundle, IRON_TOOLCHAIN_BUNDLE) == 0;
}

const char *iron_toolchain_tool(const IronToolchain *tc, const char *name) {
    static char path[4096];
#ifdef _WIN32
    snprintf(path, sizeof(path), "%s\\bin\\%s.exe", tc->root, name);
#else
    snprintf(path, sizeof(path), "%s/bin/%s", tc->root, name);
#endif
    return path;
}

/* ── Candidate locations ────────────────────────────────────────────────── */

static int prefix_candidate(char *out, size_t cap) {
    char dir[4096];
    if (self_dir(dir, sizeof(dir)) != 0) return -1;
    /* <prefix>/bin/ironc -> <prefix>/lib/iron/toolchain */
    char *last = strrchr(dir, '/');
#ifdef _WIN32
    char *lw = strrchr(dir, '\\');
    if (lw > last) last = lw;
#endif
    if (!last) return -1;
    *last = '\0';
    return path_join(out, cap, dir, "lib/iron/toolchain");
}

static int home_candidate(char *out, size_t cap) {
    const char *home = home_dir();
    if (!home) return -1;
    int n = snprintf(out, cap, "%s/.iron/toolchain/%s", home, IRON_TOOLCHAIN_VERSION);
    return (n > 0 && (size_t)n < cap) ? 0 : -1;
}

/* Check one directory. Returns 1 when it holds an acceptable bundle (tc
 * filled), 0 when there is nothing there, -1 when there is a bundle that
 * is refused (reason printed). */
static int probe(IronToolchain *tc, const char *dir, IronToolchainSource source) {
    snprintf(tc->root, sizeof(tc->root), "%s", dir);
    tc->source = source;
    if (!is_dir(dir)) return 0;
    if (read_manifest(tc) != 0) {
        if (source == IRON_TOOLCHAIN_FROM_ENV) {
            fprintf(stderr, "error: IRON_TOOLCHAIN='%s' has no %s manifest\n", dir, MANIFEST);
            return -1;
        }
        return 0;
    }
    if (!manifest_matches(tc)) {
        if (source == IRON_TOOLCHAIN_FROM_ENV) {
            fprintf(stderr, "warning: IRON_TOOLCHAIN='%s' is llvm %s bundle %s; this compiler was built for "
                            "llvm %s bundle %s\n", dir, tc->llvm, tc->bundle,
                    IRON_TOOLCHAIN_LLVM, IRON_TOOLCHAIN_BUNDLE);
        } else {
            fprintf(stderr, "error: the toolchain at %s is llvm %s bundle %s; this compiler needs "
                            "llvm %s bundle %s\n", dir, tc->llvm, tc->bundle,
                    IRON_TOOLCHAIN_LLVM, IRON_TOOLCHAIN_BUNDLE);
            return -1;
        }
    }
#ifdef _WIN32
    const char *probe_tool = "clang-cl";
#else
    const char *probe_tool = "clang";
#endif
    if (!is_file(iron_toolchain_tool(tc, probe_tool))) {
        fprintf(stderr, "error: the toolchain at %s has no bin/%s\n", dir, probe_tool);
        return -1;
    }
    return 1;
}

/* ── Download and install ───────────────────────────────────────────────── */

static int sha256_file_hex(const char *path, char out_hex[65]) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long len = ftell(f);
    if (len < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return -1; }
    uint8_t *buf = (uint8_t *)malloc((size_t)len ? (size_t)len : 1);
    if (!buf) { fclose(f); return -1; }
    if (fread(buf, 1, (size_t)len, f) != (size_t)len) { free(buf); fclose(f); return -1; }
    fclose(f);
    uint8_t digest[32];
    ilsp_sha256(buf, (size_t)len, digest);
    ilsp_sha256_hex(digest, out_hex);
    free(buf);
    return 0;
}

static void remove_tree(const char *dir) {
#ifdef _WIN32
    char *argv[] = { "cmd", "/c", "rmdir", "/s", "/q", (char *)dir, NULL };
#else
    char *argv[] = { "rm", "-rf", (char *)dir, NULL };
#endif
    run(argv);
}

/* Fetch, verify and unpack the bundle for this host into `dest`
 * (~/.iron/toolchain/<version>). */
static int install(const char *dest) {
    const char *host = iron_toolchain_host();
    const char *sha = pin_for_host(host);
    if (!sha || !*sha) {
        fprintf(stderr, "error: no toolchain bundle is published for %s (llvm %s bundle %s)\n",
                host, IRON_TOOLCHAIN_LLVM, IRON_TOOLCHAIN_BUNDLE);
        return 1;
    }
    char archive_name[256];
    snprintf(archive_name, sizeof(archive_name), "iron-toolchain-%s-%s.tar.gz", IRON_TOOLCHAIN_VERSION, host);
    const char *base = getenv("IRON_TOOLCHAIN_URL");
    if (!base || !*base) base = IRON_TOOLCHAIN_URL;
    char url[1024];
    snprintf(url, sizeof(url), "%s%s", base, archive_name);

    /* Work beside the destination so the final rename stays on one disk. */
    char parent[4096];
    snprintf(parent, sizeof(parent), "%s", dest);
    char *slash = strrchr(parent, '/');
    if (!slash) return 1;
    *slash = '\0';
    char staging[4096], archive[4096];
    snprintf(staging, sizeof(staging), "%s/.staging-%s", parent, IRON_TOOLCHAIN_VERSION);
    snprintf(archive, sizeof(archive), "%s/%s", parent, archive_name);
    remove_tree(staging);
    if (mkdir_p(staging) != 0) {
        fprintf(stderr, "error: cannot create %s: %s\n", staging, strerror(errno));
        return 1;
    }

    fprintf(stderr, "iron: downloading toolchain llvm %s bundle %s for %s\n  %s\n",
            IRON_TOOLCHAIN_LLVM, IRON_TOOLCHAIN_BUNDLE, host, url);
    char *curl[] = { "curl", "-fL", "--retry", "3", "--progress-bar", "-o", archive, url, NULL };
    int rc = run(curl);
    if (rc != 0) {
        fprintf(stderr, "error: download failed (%s); install curl or fetch the archive and unpack it into %s\n",
                rc < 0 ? "curl not found" : "curl exited with an error", dest);
        remove_tree(staging);
        return 1;
    }

    char hex[65];
    if (sha256_file_hex(archive, hex) != 0 || strcmp(hex, sha) != 0) {
        fprintf(stderr, "error: checksum mismatch for %s\n  expected %s\n  got      %s\n", archive_name, sha, hex);
        unlink(archive);
        remove_tree(staging);
        return 1;
    }

    char *tar[] = { "tar", "-xzf", archive, "-C", staging, "--strip-components=1", NULL };
    rc = run(tar);
    unlink(archive);
    if (rc != 0) {
        fprintf(stderr, "error: could not unpack %s (%s)\n", archive_name, rc < 0 ? "tar not found" : "tar failed");
        remove_tree(staging);
        return 1;
    }
    remove_tree(dest);
    if (rename(staging, dest) != 0) {
        fprintf(stderr, "error: cannot move %s to %s: %s\n", staging, dest, strerror(errno));
        remove_tree(staging);
        return 1;
    }
    fprintf(stderr, "iron: toolchain installed at %s\n", dest);
    return 0;
}

/* ── Lookup ─────────────────────────────────────────────────────────────── */

static IronToolchain s_tc;
static int s_state = 0;   /* 0 unresolved, 1 found, -1 failed */

static int resolve(bool download) {
    char dir[4096];
    const char *env = getenv("IRON_TOOLCHAIN");
    if (env && *env) {
        int r = probe(&s_tc, env, IRON_TOOLCHAIN_FROM_ENV);
        if (r == 0) fprintf(stderr, "error: IRON_TOOLCHAIN='%s' is not a directory\n", env);
        return r == 1 ? 1 : -1;
    }
    if (prefix_candidate(dir, sizeof(dir)) == 0) {
        int r = probe(&s_tc, dir, IRON_TOOLCHAIN_FROM_PREFIX);
        if (r != 0) return r;
    }
    if (home_candidate(dir, sizeof(dir)) != 0) {
        fprintf(stderr, "error: cannot locate the Iron toolchain: HOME is not set\n");
        return -1;
    }
    int r = probe(&s_tc, dir, IRON_TOOLCHAIN_FROM_HOME);
    if (r != 0) return r;
    if (!download) return 0;
    if (install(dir) != 0) return -1;
    r = probe(&s_tc, dir, IRON_TOOLCHAIN_FROM_HOME);
    if (r != 1) {
        fprintf(stderr, "error: the downloaded toolchain at %s is not usable\n", dir);
        return -1;
    }
    return 1;
}

const IronToolchain *iron_toolchain_get(bool download) {
    if (s_state == 0) {
        int r = resolve(download);
        if (r == 0) {
            fprintf(stderr, "error: the Iron toolchain (llvm %s bundle %s) is not installed; run `iron toolchain install`\n",
                    IRON_TOOLCHAIN_LLVM, IRON_TOOLCHAIN_BUNDLE);
            r = -1;
        }
        s_state = r;
    }
    return s_state == 1 ? &s_tc : NULL;
}

static const char *source_name(IronToolchainSource s) {
    switch (s) {
    case IRON_TOOLCHAIN_FROM_ENV:    return "IRON_TOOLCHAIN";
    case IRON_TOOLCHAIN_FROM_PREFIX: return "installed";
    case IRON_TOOLCHAIN_FROM_HOME:   return "user";
    }
    return "user";
}

void iron_toolchain_print_version(FILE *out) {
    IronToolchain tc;
    char dir[4096];
    const char *env = getenv("IRON_TOOLCHAIN");
    int r = 0;
    if (env && *env) {
        r = probe(&tc, env, IRON_TOOLCHAIN_FROM_ENV);
    } else {
        if (prefix_candidate(dir, sizeof(dir)) == 0) r = probe(&tc, dir, IRON_TOOLCHAIN_FROM_PREFIX);
        if (r == 0 && home_candidate(dir, sizeof(dir)) == 0) r = probe(&tc, dir, IRON_TOOLCHAIN_FROM_HOME);
    }
    if (r == 1)
        fprintf(out, "toolchain llvm %s bundle %s (%s, %s)\n", tc.llvm, tc.bundle, source_name(tc.source), tc.root);
    else
        fprintf(out, "toolchain llvm %s bundle %s (%s)\n", IRON_TOOLCHAIN_LLVM, IRON_TOOLCHAIN_BUNDLE,
                r == 0 ? "not installed; run `iron toolchain install`" : "refused");
}

/* ── `ironc toolchain` ──────────────────────────────────────────────────── */

int iron_toolchain_cmd(int argc, char **argv) {
    const char *sub = argc > 1 ? argv[1] : "info";
    if (strcmp(sub, "install") == 0) {
        char dir[4096];
        if (home_candidate(dir, sizeof(dir)) != 0) {
            fprintf(stderr, "error: HOME is not set\n");
            return 1;
        }
        IronToolchain tc;
        if (probe(&tc, dir, IRON_TOOLCHAIN_FROM_HOME) == 1) {
            fprintf(stderr, "iron: toolchain llvm %s bundle %s is already installed at %s\n",
                    tc.llvm, tc.bundle, tc.root);
            return 0;
        }
        return install(dir);
    }
    if (strcmp(sub, "path") == 0) {
        const IronToolchain *tc = iron_toolchain_get(false);
        if (!tc) return 1;
        printf("%s\n", tc->root);
        return 0;
    }
    if (strcmp(sub, "info") == 0) {
        iron_toolchain_print_version(stdout);
        printf("host %s\n", iron_toolchain_host());
        const char *sha = pin_for_host(iron_toolchain_host());
        printf("archive %siron-toolchain-%s-%s.tar.gz\nsha256 %s\n", IRON_TOOLCHAIN_URL, IRON_TOOLCHAIN_VERSION,
               iron_toolchain_host(), (sha && *sha) ? sha : "(not published)");
        return 0;
    }
    fprintf(stderr, "usage: iron toolchain [info|path|install]\n");
    return 1;
}
