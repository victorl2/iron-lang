/* iron_win_crt0.c: process entry for Windows programs built against a
 * runtime bundle (scripts/rt/build_windows_bundle.sh).
 *
 * A program built with MSVC gets its entry point and a few support symbols
 * from vcruntime and the static CRT startup objects, which ship only with
 * the Visual Studio Build Tools. Everything else a program uses comes from
 * DLLs that are part of Windows 10 and later (ucrtbase, kernel32, ws2_32,
 * bcrypt). This file supplies the missing pieces so a program links with
 * lld-link against import libraries alone:
 *
 *   mainCRTStartup  sets up the UCRT's argv and environment, calls main and
 *                   exits through the UCRT so atexit handlers and stdio
 *                   flushing run;
 *   atexit          registers with the UCRT's process-wide table (and
 *                   _onexit, its form in code built against the static CRT);
 *   _tls_used       the TLS directory the loader reads (_Thread_local);
 *   _fltused        referenced by every object that uses floating point;
 *   __chkstk        the stack probe clang emits for frames over a page.
 *
 * Compiled for x86_64-pc-windows-msvc only; nothing here is used on other
 * targets or by native builds that link with the Build Tools. */
#if defined(_WIN32) && defined(_M_X64)

#include <stdint.h>

int main(int argc, char **argv);

__declspec(dllimport) void _set_app_type(int type);
__declspec(dllimport) int _configure_narrow_argv(int mode);
__declspec(dllimport) int _initialize_narrow_environment(void);
__declspec(dllimport) int *__p___argc(void);
__declspec(dllimport) char ***__p___argv(void);
__declspec(dllimport) int _crt_atexit(void (*fn)(void));
__declspec(dllimport) __declspec(noreturn) void exit(int code);

enum { IRON_CRT_CONSOLE_APP = 1, IRON_CRT_ARGV_UNEXPANDED = 1 };

void mainCRTStartup(void) {
    _set_app_type(IRON_CRT_CONSOLE_APP);
    _configure_narrow_argv(IRON_CRT_ARGV_UNEXPANDED);
    _initialize_narrow_environment();
    exit(main(*__p___argc(), *__p___argv()));
}

int atexit(void (*fn)(void)) {
    return _crt_atexit(fn) == 0 ? 0 : -1;
}

/* What atexit compiles to in code built against the static CRT (/MT), as
 * the bundle's OpenSSL is. The handler's int result is ignored, which the
 * x64 calling convention allows. */
typedef int (*iron_onexit_t)(void);
iron_onexit_t _onexit(iron_onexit_t fn) {
    return _crt_atexit((void (*)(void))fn) == 0 ? fn : 0;
}

int _fltused = 0x9875;

/* Thread-local storage. The linker sorts .tls sections by the suffix after
 * '$', and the compiler puts _Thread_local variables in ".tls$" (empty
 * suffix), so the start marker goes in plain ".tls", which sorts before
 * them, and the end marker in ".tls$ZZZ" after them. Everything between is
 * the template the loader copies for each thread; the PE TLS directory
 * entry is filled from _tls_used. */
#pragma section(".tls", read, write)
#pragma section(".tls$ZZZ", read, write)
#pragma section(".CRT$XLA", read)
#pragma section(".CRT$XLZ", read)

__declspec(allocate(".tls")) char _tls_start = 0;
__declspec(allocate(".tls$ZZZ")) char _tls_end = 0;

typedef void (__stdcall *iron_tls_callback)(void *, unsigned long, void *);
__declspec(allocate(".CRT$XLA")) const iron_tls_callback __xl_a = 0;
__declspec(allocate(".CRT$XLZ")) const iron_tls_callback __xl_z = 0;

unsigned long _tls_index = 0;

/* IMAGE_TLS_DIRECTORY64, spelled out so no SDK header is needed. */
typedef struct {
    uint64_t start_address_of_raw_data;
    uint64_t end_address_of_raw_data;
    uint64_t address_of_index;
    uint64_t address_of_callbacks;
    uint32_t size_of_zero_fill;
    uint32_t characteristics;
} iron_tls_directory;

const iron_tls_directory _tls_used = {
    (uint64_t)&_tls_start,
    (uint64_t)&_tls_end,
    (uint64_t)&_tls_index,
    (uint64_t)(&__xl_a + 1),
    0,
    0,
};

/* __chkstk: touch each page between the current stack limit and the new
 * stack pointer (rsp - rax) so the guard page grows the stack in order.
 * rax holds the frame size; every register is preserved. */
__asm__(
    ".text\n"
    ".globl __chkstk\n"
    "__chkstk:\n"
    "    pushq %rcx\n"
    "    pushq %rax\n"
    "    cmpq  $0x1000, %rax\n"
    "    leaq  24(%rsp), %rcx\n"
    "    jb    2f\n"
    "1:\n"
    "    subq  $0x1000, %rcx\n"
    "    testq %rcx, (%rcx)\n"
    "    subq  $0x1000, %rax\n"
    "    cmpq  $0x1000, %rax\n"
    "    ja    1b\n"
    "2:\n"
    "    subq  %rax, %rcx\n"
    "    testq %rcx, (%rcx)\n"
    "    popq  %rax\n"
    "    popq  %rcx\n"
    "    ret\n");

#endif /* _WIN32 && _M_X64 */
