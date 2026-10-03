/* prereqs.h: what the host still has to provide for a build, and how
 * ironc explains it when it is missing.
 *
 * The pinned toolchain brings the C compiler, but until the runtime ships
 * precompiled (#237) the runtime sources are compiled on the user's
 * machine against the platform's C library headers and link inputs:
 *
 *   Windows  the Visual Studio Build Tools (MSVC and the Windows SDK),
 *            found by clang-cl on its own
 *   macOS    the Xcode command line tools (the SDK, through xcrun)
 *   Linux    the C library development headers (libc6-dev, glibc-devel)
 *
 * Like rustup, ironc does not leave the user with a compiler error about
 * stdio.h: the compiler's stderr is captured, the failure is recognized,
 * the missing piece is named, and when run from a terminal ironc offers to
 * run the installer. `iron toolchain check` runs the probe on demand. */
#ifndef IRON_CLI_PREREQS_H
#define IRON_CLI_PREREQS_H

#include <stdbool.h>

/* The C compiler's stderr goes to a temporary file so it can be read back
 * after the process ends. open() creates it; the spawn redirects fd 2 (or
 * the Windows standard error handle) into it; finish() replays it to our
 * stderr, and when the compiler failed, explains a missing prerequisite
 * if one is recognized. Returns true from finish() when it printed such
 * an explanation. */
typedef struct {
    char path[4096];
    int  fd;           /* POSIX: an open descriptor for the spawn's fd 2 */
#ifdef _WIN32
    void *handle;      /* HANDLE for STARTUPINFO.hStdError */
#endif
} IronCcCapture;

bool iron_cc_capture_open(IronCcCapture *c);
bool iron_cc_capture_finish(IronCcCapture *c, bool cc_failed);

/* Compile and link a one-line program with the toolchain and report
 * whether the platform prerequisites are present. Offers the installer
 * when something is missing and stdin is a terminal. Returns 0 when the
 * probe built, 1 otherwise. */
int iron_prereqs_check(void);

#endif /* IRON_CLI_PREREQS_H */
