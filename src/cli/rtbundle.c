#include "cli/rtbundle.h"
#include "cli/toolchain.h"
#include "cli/version.h"
#include "util/os.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef IRON_RT_URL
/* Base URL of the release assets; the archive name is appended. */
#define IRON_RT_URL "https://github.com/victorl2/iron-lang/releases/download/v" IRON_VERSION_STRING "/"
#endif

static void native_separators(char *p) {
#ifdef _WIN32
    for (; *p; p++) if (*p == '/') *p = '\\';
#else
    (void)p;
#endif
}

/* Read rt.txt. 1 when the bundle fits this ironc and target, 0 when the
 * directory holds no bundle, -1 when it holds one that is refused. */
static int probe(IronRtBundle *b, const char *dir, const IronCrossTarget *target, bool is_override) {
    char manifest[4096];
    snprintf(manifest, sizeof(manifest), "%s/rt.txt", dir);
    native_separators(manifest);
    FILE *f = fopen(manifest, "r");
    if (!f) return 0;
    char key[32], v1[128], v2[128];
    char version[64] = "", tname[64] = "", libc[64] = "", commit[64] = "";
    bool magic = false;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        int n = sscanf(line, "%31s %127s %127s", key, v1, v2);
        if (n < 2) continue;
        if (strcmp(key, "iron-rt") == 0) magic = strcmp(v1, "1") == 0;
        else if (strcmp(key, "version") == 0) snprintf(version, sizeof(version), "%s", v1);
        else if (strcmp(key, "target") == 0) snprintf(tname, sizeof(tname), "%s", v1);
        else if (strcmp(key, "commit") == 0) snprintf(commit, sizeof(commit), "%s", v1);
        else if (strcmp(key, "libc") == 0) snprintf(libc, sizeof(libc), "%s%s%s", v1, n > 2 ? " " : "", n > 2 ? v2 : "");
    }
    fclose(f);
    if (!magic) {
        fprintf(stderr, "error: %s is not an Iron runtime bundle manifest\n", manifest);
        return -1;
    }
    if (strcmp(tname, target->name) != 0) {
        fprintf(stderr, "error: the runtime bundle at %s is for %s, not %s\n", dir, tname, target->name);
        return -1;
    }
    /* The runtime and the generated C must come from the same compiler: a
     * bundle from another version or commit is refused (the developer
     * override only warns). */
    bool same = strcmp(version, IRON_VERSION_STRING) == 0 &&
                (strcmp(commit, IRON_GIT_HASH) == 0 || strcmp(IRON_GIT_HASH, "unknown") == 0 ||
                 strcmp(commit, "unknown") == 0);
    if (!same) {
        if (is_override) {
            fprintf(stderr, "warning: IRON_RT_DIR bundle for %s is iron %s (%s); this ironc is %s (%s)\n",
                    target->name, version, commit, IRON_VERSION_STRING, IRON_GIT_HASH);
        } else {
            fprintf(stderr, "error: the runtime bundle at %s is iron %s (%s); this ironc is %s (%s)\n",
                    dir, version, commit, IRON_VERSION_STRING, IRON_GIT_HASH);
            return -1;
        }
    }
    snprintf(b->root, sizeof(b->root), "%s", dir);
    snprintf(b->lib, sizeof(b->lib), "%s/lib", dir);
    native_separators(b->lib);
    snprintf(b->version, sizeof(b->version), "%s", version);
    snprintf(b->libc, sizeof(b->libc), "%s", libc);
    b->target = target;
    return 1;
}

static int install(const char *dest, const IronCrossTarget *target) {
    char archive_name[256];
    snprintf(archive_name, sizeof(archive_name), "iron-rt-%s-%s.tar.gz", IRON_VERSION_STRING, target->name);
    const char *base = getenv("IRON_RT_URL");
    if (!base || !*base) base = IRON_RT_URL;
    char url[1024], sidecar[1100], label[256];
    snprintf(url, sizeof(url), "%s%s", base, archive_name);
    snprintf(sidecar, sizeof(sidecar), "%s.sha256", url);
    snprintf(label, sizeof(label), "runtime for %s (iron %s)", target->name, IRON_VERSION_STRING);
    if (iron_toolchain_fetch_archive(url, archive_name, NULL, sidecar, dest, label) != 0) return 1;
    fprintf(stderr, "iron: runtime for %s installed at %s\n", target->name, dest);
    return 0;
}

#define MAX_BUNDLES 8
static IronRtBundle s_bundles[MAX_BUNDLES];
static int s_state[MAX_BUNDLES];   /* 0 unresolved, 1 found, -1 failed */

static const IronRtBundle *lookup(const IronCrossTarget *target, bool download, bool quiet);

const IronRtBundle *iron_rt_bundle_get(const IronCrossTarget *target, bool download) {
    return lookup(target, download, false);
}

const IronRtBundle *iron_rt_bundle_find(const IronCrossTarget *target) {
    return lookup(target, false, true);
}

static const IronRtBundle *lookup(const IronCrossTarget *target, bool download, bool quiet) {
    if (!target) return NULL;
    int slot = -1;
    for (int i = 0; i < MAX_BUNDLES; i++) {
        if (s_state[i] != 0 && s_bundles[i].target == target) return s_state[i] == 1 ? &s_bundles[i] : NULL;
        if (s_state[i] == 0 && slot < 0) slot = i;
    }
    if (slot < 0) return NULL;
    IronRtBundle *b = &s_bundles[slot];
    memset(b, 0, sizeof(*b));
    b->target = target;
    char dir[4096];
    int r = 0;

    const char *env = getenv("IRON_RT_DIR");
    if (env && *env) {
        snprintf(dir, sizeof(dir), "%s/%s", env, target->name);
        native_separators(dir);
        r = probe(b, dir, target, true);
        if (r == 0) fprintf(stderr, "error: IRON_RT_DIR='%s' holds no runtime bundle for %s\n", env, target->name);
        s_state[slot] = r == 1 ? 1 : -1;
        return r == 1 ? b : NULL;
    }
    char prefix[4096];
    if (iron_toolchain_prefix_dir(prefix, sizeof(prefix)) == 0) {
        snprintf(dir, sizeof(dir), "%s/lib/iron/rt/%s", prefix, target->name);
        native_separators(dir);
        r = probe(b, dir, target, false);
        if (r != 0) { s_state[slot] = r; return r == 1 ? b : NULL; }
    }
    const char *home = iron_toolchain_home_dir();
    if (!home) {
        fprintf(stderr, "error: cannot locate the runtime bundle for %s: HOME is not set\n", target->name);
        s_state[slot] = -1;
        return NULL;
    }
    snprintf(dir, sizeof(dir), "%s/.iron/rt/%s/%s", home, IRON_VERSION_STRING, target->name);
    native_separators(dir);
    r = probe(b, dir, target, false);
    if (r == 0 && download) {
        if (!target->available) {
            fprintf(stderr, "error: no runtime bundle is published for %s yet\n", target->name);
            s_state[slot] = -1;
            return NULL;
        }
        if (install(dir, target) == 0) r = probe(b, dir, target, false);
        if (r != 1) {
            if (r == 0) fprintf(stderr, "error: the downloaded runtime bundle at %s is not usable\n", dir);
            r = -1;
        }
    } else if (r == 0) {
        if (!quiet)
            fprintf(stderr, "error: the runtime bundle for %s (iron %s) is not installed\n", target->name, IRON_VERSION_STRING);
        r = -1;
    }
    s_state[slot] = r;
    return r == 1 ? b : NULL;
}
