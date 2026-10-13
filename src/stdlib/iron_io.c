#include "iron_io.h"
#include "runtime/iron_errors.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include "util/os.h"   /* dirent, mkdir, access on every platform */

/* A C copy of a path, or NULL (errno EINVAL) for an empty path or one
 * holding a zero byte: such a path cannot name a file, and handing its
 * prefix to the OS would act on a different file (#568). */
static char *io_path_cstr(Iron_String path) {
    size_t length = iron_string_byte_len(&path);
    const char *bytes = iron_string_cstr(&path);
    if (length == 0 || memchr(bytes, '\0', length) != NULL) {
        errno = EINVAL;
        return NULL;
    }
    char *copy = (char *)malloc(length + 1);
    if (!copy) {
        errno = ENOMEM;
        return NULL;
    }
    memcpy(copy, bytes, length);
    copy[length] = '\0';
    return copy;
}

static FILE *io_fopen(Iron_String path, const char *mode) {
    char *p = io_path_cstr(path);
    if (!p) return NULL;
    FILE *f = fopen(p, mode);
    int saved = errno;
    free(p);
    errno = saved;
    return f;
}

static Iron_FileReadResult io_read_bounded(Iron_String path, int64_t max_bytes);

/* ── File I/O ────────────────────────────────────────────────────────────── */

Iron_Result_String_Error Iron_io_read_file_result(Iron_String path) {
    Iron_FileReadResult r = io_read_bounded(path, (int64_t)(SIZE_MAX / 2));
    iron_string_release(&r.error_message);
    if (r.error != 0) {
        iron_string_release(&r.data);
        Iron_String empty = iron_string_from_cstr("", 0);
        return (Iron_Result_String_Error){ empty, iron_error_new(1, "file not found") };
    }
    return (Iron_Result_String_Error){ r.data, iron_error_none() };
}

static Iron_Error io_put(Iron_String path, const uint8_t *data, size_t len,
                         const char *mode) {
    FILE *f = io_fopen(path, mode);
    if (!f) return iron_error_new(1, "could not open file for writing");
    size_t written = len ? fwrite(data, 1, len, f) : 0;
    int failed = written != len || fflush(f) != 0;
    if (fclose(f) != 0) failed = 1;
    if (failed) return iron_error_new(5, "write incomplete");
    return iron_error_none();
}

Iron_Error Iron_io_write_file(Iron_String path, Iron_String content) {
    return io_put(path, (const uint8_t *)iron_string_cstr(&content),
                  iron_string_byte_len(&content), "wb");
}

Iron_Result_String_Error Iron_io_read_bytes_result(Iron_String path) {
    /* Same as read_file — binary mode is already used */
    return Iron_io_read_file_result(path);
}

Iron_Error Iron_io_write_bytes_raw(Iron_String path, const uint8_t *data, size_t len) {
    return io_put(path, data, len, "wb");
}

/* ── File system ─────────────────────────────────────────────────────────── */

bool Iron_io_file_exists(Iron_String path) {
    char *p = io_path_cstr(path);
    if (!p) return false;
    struct stat st;
    bool exists = stat(p, &st) == 0;
    free(p);
    return exists;
}

Iron_Error Iron_io_create_dir(Iron_String path) {
    char *p = io_path_cstr(path);
    if (!p) return iron_error_new(6, "could not create directory");
    /* WINDOWS-TODO: mkdir(path, mode) is POSIX. On Windows use _mkdir(path) from <direct.h> (no mode arg). */
    int result = mkdir(p, 0755);
    int saved = errno;
    free(p);
    if (result != 0 && saved != EEXIST) {
        return iron_error_new(6, "could not create directory");
    }
    return iron_error_none();
}

Iron_Error Iron_io_delete_file(Iron_String path) {
    char *p = io_path_cstr(path);
    if (!p) return iron_error_new(7, "could not delete file");
    int result = remove(p);
    free(p);
    if (result != 0) {
        return iron_error_new(7, "could not delete file");
    }
    return iron_error_none();
}

Iron_Result_String_Error Iron_io_list_files_result(Iron_String dir_path) {
    char *p = io_path_cstr(dir_path);
    /* WINDOWS-TODO: opendir/readdir/closedir are POSIX. On Windows replace this block with
     * FindFirstFileA/FindNextFileA/FindClose from <windows.h>. WIN32_FIND_DATAA carries
     * the entry name in cFileName. */
    DIR *dir = p ? opendir(p) : NULL;
    free(p);
    if (!dir) {
        Iron_String empty = iron_string_from_cstr("", 0);
        return (Iron_Result_String_Error){ empty, iron_error_new(8, "could not open directory") };
    }

    /* Build a newline-separated list of filenames */
    size_t buf_cap = 1024;
    size_t buf_len = 0;
    char *buf = (char *)malloc(buf_cap);
    if (!buf) {
        closedir(dir);
        Iron_String empty = iron_string_from_cstr("", 0);
        return (Iron_Result_String_Error){ empty, iron_error_new(4, "out of memory") };
    }
    buf[0] = '\0';

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        /* Skip . and .. */
        if (entry->d_name[0] == '.' &&
            (entry->d_name[1] == '\0' ||
             (entry->d_name[1] == '.' && entry->d_name[2] == '\0'))) {
            continue;
        }

        size_t name_len = strlen(entry->d_name);
        size_t needed = buf_len + name_len + 2; /* name + newline + null */
        if (needed > buf_cap) {
            buf_cap = needed * 2;
            char *new_buf = (char *)realloc(buf, buf_cap);
            if (!new_buf) {
                free(buf);
                closedir(dir);
                Iron_String empty = iron_string_from_cstr("", 0);
                return (Iron_Result_String_Error){ empty, iron_error_new(4, "out of memory") };
            }
            buf = new_buf;
        }

        memcpy(buf + buf_len, entry->d_name, name_len);
        buf_len += name_len;
        buf[buf_len++] = '\n';
        buf[buf_len] = '\0';
    }
    closedir(dir);

    Iron_String result = iron_string_from_cstr(buf, buf_len);
    free(buf);

    return (Iron_Result_String_Error){ result, iron_error_none() };
}

/* ── Phase 39 additions ──────────────────────────────────────────────────── */

/* One line of standard input without its line ending (LF or CRLF), of any
 * length and with its bytes as read (a zero byte included); "" at end of
 * input. */
Iron_String Iron_io_read_line(void) {
    size_t cap = 256, len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) return iron_string_from_cstr("", 0);
    int c;
    while ((c = getchar()) != EOF && c != '\n') {
        if (len + 1 >= cap) {
            char *grown = (char *)realloc(buf, cap * 2);
            if (!grown) break;
            buf = grown;
            cap *= 2;
        }
        buf[len++] = (char)c;
    }
    if (len > 0 && buf[len - 1] == '\r') len--;
    Iron_String line = iron_string_from_cstr(buf, len);
    free(buf);
    return line;
}

/* Unbuffered whole-string writes to fd 1 / fd 2 (IO.write_stdout and
 * IO.write_stderr). stdout's stdio buffer is flushed first so output from
 * println and from these calls stays in program order. */
static int64_t io_write_fd(int fd, Iron_String s) {
    const char *data = iron_string_cstr(&s);
    size_t len = iron_string_byte_len(&s);
    fflush(stdout);
    size_t done = 0;
    while (done < len) {
#ifdef _WIN32
        size_t chunk = len - done > 0x7fffffff ? 0x7fffffff : len - done;
        int n = _write(fd, data + done, (unsigned int)chunk);
#else
        ssize_t n = write(fd, data + done, len - done);
#endif
        if (n < 0) {
#ifndef _WIN32
            if (errno == EINTR) continue;
#endif
            return -1;
        }
        if (n == 0) return -1;
        done += (size_t)n;
    }
    return (int64_t)done;
}

int64_t Iron_io_write_stdout(Iron_String s) { return io_write_fd(1, s); }
int64_t Iron_io_write_stderr(Iron_String s) { return io_write_fd(2, s); }

Iron_Error Iron_io_append_file(Iron_String path, Iron_String content) {
    return io_put(path, (const uint8_t *)iron_string_cstr(&content),
                  iron_string_byte_len(&content), "ab");
}

/* The path helpers work on the bytes of the path (a zero byte is an
 * ordinary byte here, not the end), always with '/' as the separator. */
static ptrdiff_t io_last_slash(const char *p, size_t n) {
    for (size_t i = n; i > 0; i--) {
        if (p[i - 1] == '/') return (ptrdiff_t)(i - 1);
    }
    return -1;
}

Iron_String Iron_io_basename(Iron_String path) {
    const char *p = iron_string_cstr(&path);
    size_t n = iron_string_byte_len(&path);
    ptrdiff_t last = io_last_slash(p, n);
    size_t start = last < 0 ? 0 : (size_t)last + 1;
    return iron_string_from_cstr(p + start, n - start);
}

Iron_String Iron_io_dirname(Iron_String path) {
    const char *p = iron_string_cstr(&path);
    size_t n = iron_string_byte_len(&path);
    ptrdiff_t last = io_last_slash(p, n);
    if (last < 0) return iron_string_from_cstr(".", 1);
    if (last == 0) return iron_string_from_cstr("/", 1);
    return iron_string_from_cstr(p, (size_t)last);
}

Iron_String Iron_io_join_path(Iron_String a, Iron_String b) {
    /* Always forward slash — works on Windows too (locked decision). An
     * empty first part gives the second part unchanged: joining onto ""
     * must not turn a relative path into an absolute one. */
    const char *ap = iron_string_cstr(&a);
    const char *bp = iron_string_cstr(&b);
    size_t alen = iron_string_byte_len(&a);
    size_t blen = iron_string_byte_len(&b);
    if (alen == 0) return iron_string_from_cstr(bp, blen);
    /* Strip trailing slash(es) from a */
    while (alen > 0 && ap[alen - 1] == '/') alen--;
    size_t total = alen + 1 + blen + 1;
    char *buf = (char *)malloc(total);
    if (!buf) return iron_string_from_cstr("", 0);
    memcpy(buf, ap, alen);
    buf[alen] = '/';
    memcpy(buf + alen + 1, bp, blen);
    buf[alen + 1 + blen] = '\0';
    Iron_String result = iron_string_from_cstr(buf, alen + 1 + blen);
    free(buf);
    return result;
}

Iron_String Iron_io_extension(Iron_String path) {
    /* Returns extension WITHOUT the leading dot: "file.iron" -> "iron" */
    const char *p = iron_string_cstr(&path);
    size_t n = iron_string_byte_len(&path);
    ptrdiff_t last = io_last_slash(p, n);
    size_t base = last < 0 ? 0 : (size_t)last + 1;
    size_t dot = n;
    for (size_t i = n; i > base; i--) {
        if (p[i - 1] == '.') { dot = i - 1; break; }
    }
    /* No dot, or dot is the first char of filename (e.g. ".gitignore") */
    if (dot == n || dot == base) return iron_string_from_cstr("", 0);
    return iron_string_from_cstr(p + dot + 1, n - dot - 1);
}

bool Iron_io_is_dir(Iron_String path) {
    char *p = io_path_cstr(path);
    if (!p) return false;
    struct stat st;
    bool dir = stat(p, &st) == 0 && S_ISDIR(st.st_mode);
    free(p);
    return dir;
}

Iron_List_Iron_String Iron_io_read_lines(Iron_String path) {
    /* Read file content then split on newline. Strip trailing empty element. */
    Iron_Result_String_Error res = Iron_io_read_file_result(path);
    if (!iron_error_is_ok(res.v1)) {
        return Iron_List_Iron_String_create();
    }
    Iron_String sep = iron_string_from_cstr("\n", 1);
    Iron_List_Iron_String lines = Iron_string_split(res.v0, sep);
    iron_string_release(&res.v0);
    /* Remove trailing empty string if file ended with \n (common case) */
    if (lines.count > 0) {
        Iron_String last = lines.items[lines.count - 1];
        if (iron_string_byte_len(&last) == 0) {
            iron_string_release(&lines.items[lines.count - 1]);
            lines.count--;
        }
    }
    return lines;
}

/* ── Bounded, binary-safe file API ─────────────────────────────────────── */

static const char *io_error_message(int64_t code) {
    switch (code) {
        case 0: return "";
        case IRON_ERR_IO_NOT_FOUND: return "file not found";
        case IRON_ERR_IO_PERMISSION: return "file permission denied";
        case IRON_ERR_IO_INVALID_ARGUMENT: return "invalid file argument";
        case IRON_ERR_IO_TOO_LARGE: return "file exceeds byte limit";
        case IRON_ERR_IO_READ: return "file read failed";
        case IRON_ERR_IO_WRITE: return "file write failed";
        case IRON_ERR_IO_SEEK: return "file seek failed";
        case IRON_ERR_IO_ALREADY_EXISTS: return "destination already exists";
        case IRON_ERR_IO_NOT_DIRECTORY: return "path is not a directory";
        case IRON_ERR_IO_IS_DIRECTORY: return "path is a directory";
        case IRON_ERR_IO_NO_MEMORY: return "out of memory while processing file";
        default: return "file operation failed";
    }
}

static int64_t io_error_from_errno(int error) {
    switch (error) {
        case ENOENT: return IRON_ERR_IO_NOT_FOUND;
        case EACCES:
        case EPERM: return IRON_ERR_IO_PERMISSION;
        case EEXIST: return IRON_ERR_IO_ALREADY_EXISTS;
        case ENOTDIR: return IRON_ERR_IO_NOT_DIRECTORY;
        case EISDIR: return IRON_ERR_IO_IS_DIRECTORY;
        case EINVAL:
        case ENAMETOOLONG: return IRON_ERR_IO_INVALID_ARGUMENT;
        default: return IRON_ERR_IO_OTHER;
    }
}

static Iron_String io_message(int64_t code) {
    const char *message = io_error_message(code);
    return iron_string_from_cstr(message, strlen(message));
}

static Iron_FileReadResult io_read_error(int64_t code) {
    Iron_FileReadResult out;
    out.data = iron_string_from_cstr("", 0);
    out.error = code;
    out.error_message = io_message(code);
    return out;
}

static Iron_FileWriteResult io_write_result(int64_t bytes, int64_t code) {
    Iron_FileWriteResult out;
    out.bytes = bytes;
    out.error = code;
    out.error_message = io_message(code);
    return out;
}

void Iron_filereadresult_release(Iron_FileReadResult result) {
    /* Strings are released by the value's own drop glue (#182); kept
     * for source compatibility. */
    (void)result;
}

void Iron_filewriteresult_release(Iron_FileWriteResult result) {
    /* Strings are released by the value's own drop glue (#182); kept
     * for source compatibility. */
    (void)result;
}

void Iron_fileinfo_release(Iron_FileInfo info) {
    /* Strings are released by the value's own drop glue (#182); kept
     * for source compatibility. */
    (void)info;
}

static int io_path_copy(Iron_String path, char **output) {
    size_t length = iron_string_byte_len(&path);
    const char *bytes = iron_string_cstr(&path);
    if (length == 0 || memchr(bytes, '\0', length) != NULL) return 0;
    char *copy = (char *)malloc(length + 1);
    if (!copy) return -1;
    memcpy(copy, bytes, length);
    copy[length] = '\0';
    *output = copy;
    return 1;
}

/* Read an open stream to its end, failing with TOO_LARGE past max_bytes.
 * Closes the stream. */
static Iron_FileReadResult io_read_stream(FILE *file, int64_t max_bytes) {
    size_t cap = 4096, length = 0;
    uint8_t *buffer = (uint8_t *)malloc(cap);
    if (!buffer) {
        fclose(file);
        return io_read_error(IRON_ERR_IO_NO_MEMORY);
    }
    for (;;) {
        if (length == cap) {
            uint8_t *grown = (uint8_t *)realloc(buffer, cap * 2);
            if (!grown) {
                free(buffer);
                fclose(file);
                return io_read_error(IRON_ERR_IO_NO_MEMORY);
            }
            buffer = grown;
            cap *= 2;
        }
        size_t got = fread(buffer + length, 1, cap - length, file);
        length += got;
        if ((uint64_t)length > (uint64_t)max_bytes) {
            free(buffer);
            fclose(file);
            return io_read_error(IRON_ERR_IO_TOO_LARGE);
        }
        if (got == 0) break;
    }
    int failed = ferror(file);
    if (fclose(file) != 0 || failed) {
        free(buffer);
        return io_read_error(IRON_ERR_IO_READ);
    }
    Iron_FileReadResult out;
    out.data = iron_string_from_cstr((const char *)buffer, length);
    out.error = 0;
    out.error_message = io_message(0);
    free(buffer);
    return out;
}

static Iron_FileReadResult io_read_bounded(Iron_String path,
                                            int64_t max_bytes) {
    if (max_bytes < 0 || (uint64_t)max_bytes > (uint64_t)(SIZE_MAX - 1)) {
        return io_read_error(IRON_ERR_IO_INVALID_ARGUMENT);
    }
    char *path_text = NULL;
    int path_status = io_path_copy(path, &path_text);
    if (path_status <= 0) {
        return io_read_error(path_status < 0 ? IRON_ERR_IO_NO_MEMORY
                                             : IRON_ERR_IO_INVALID_ARGUMENT);
    }
    FILE *file = fopen(path_text, "rb");
    if (!file) {
        int64_t code = io_error_from_errno(errno);
        free(path_text);
        return io_read_error(code);
    }
    free(path_text);
    /* fopen succeeds on a directory on POSIX systems; reading it is an
     * error of its own. A file whose size the OS does not report (a pipe,
     * a /proc or /sys file) is read to its end, still within max_bytes. */
    struct stat opened;
    if (fstat(fileno(file), &opened) == 0 && !S_ISREG(opened.st_mode)) {
        if (S_ISDIR(opened.st_mode)) {
            fclose(file);
            return io_read_error(IRON_ERR_IO_IS_DIRECTORY);
        }
        return io_read_stream(file, max_bytes);
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return io_read_error(IRON_ERR_IO_SEEK);
    }
    long end = ftell(file);
    if (end < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return io_read_error(IRON_ERR_IO_SEEK);
    }
    if ((uint64_t)end > (uint64_t)max_bytes) {
        fclose(file);
        return io_read_error(IRON_ERR_IO_TOO_LARGE);
    }
    size_t length = (size_t)end;
    uint8_t *buffer = (uint8_t *)malloc(length == 0 ? 1 : length);
    if (!buffer) {
        fclose(file);
        return io_read_error(IRON_ERR_IO_NO_MEMORY);
    }
    size_t received = fread(buffer, 1, length, file);
    int read_failed = received != length || ferror(file);
    int close_failed = fclose(file) != 0;
    if (read_failed || close_failed) {
        free(buffer);
        return io_read_error(IRON_ERR_IO_READ);
    }
    Iron_FileReadResult out;
    out.data = iron_string_from_cstr((const char *)buffer, length);
    out.error = 0;
    out.error_message = io_message(0);
    free(buffer);
    return out;
}

Iron_FileReadResult Iron_io_read_text(Iron_String path, int64_t max_bytes) {
    return io_read_bounded(path, max_bytes);
}

Iron_FileReadResult Iron_io_read_bytes(Iron_String path, int64_t max_bytes) {
    return io_read_bounded(path, max_bytes);
}

static Iron_FileWriteResult io_write_string(Iron_String path,
                                             Iron_String content,
                                             const char *mode) {
    char *path_text = NULL;
    int path_status = io_path_copy(path, &path_text);
    if (path_status <= 0) {
        return io_write_result(0, path_status < 0 ? IRON_ERR_IO_NO_MEMORY
                                                  : IRON_ERR_IO_INVALID_ARGUMENT);
    }
    FILE *file = fopen(path_text, mode);
    if (!file) {
        int64_t code = io_error_from_errno(errno);
        free(path_text);
        return io_write_result(0, code);
    }
    free(path_text);
    const uint8_t *data = (const uint8_t *)iron_string_cstr(&content);
    size_t length = iron_string_byte_len(&content);
    size_t written = 0;
    while (written < length) {
        size_t count = fwrite(data + written, 1, length - written, file);
        if (count == 0) break;
        written += count;
    }
    int failed = written != length || ferror(file) || fflush(file) != 0;
    if (fclose(file) != 0) failed = 1;
    return io_write_result((int64_t)written,
                           failed ? IRON_ERR_IO_WRITE : 0);
}

Iron_FileWriteResult Iron_io_write_text(Iron_String path, Iron_String content) {
    return io_write_string(path, content, "wb");
}

Iron_FileWriteResult Iron_io_write_bytes(Iron_String path, Iron_String content) {
    return io_write_string(path, content, "wb");
}

Iron_FileWriteResult Iron_io_append_text(Iron_String path, Iron_String content) {
    return io_write_string(path, content, "ab");
}

Iron_FileWriteResult Iron_io_append_bytes(Iron_String path, Iron_String content) {
    return io_write_string(path, content, "ab");
}

Iron_FileInfo Iron_io_file_info(Iron_String path) {
    Iron_FileInfo out;
    memset(&out, 0, sizeof(out));
    out.error_message = io_message(0);
    char *path_text = NULL;
    int path_status = io_path_copy(path, &path_text);
    if (path_status <= 0) {
        out.error = path_status < 0 ? IRON_ERR_IO_NO_MEMORY
                                    : IRON_ERR_IO_INVALID_ARGUMENT;
        out.error_message = io_message(out.error);
        return out;
    }
    struct stat status;
    if (stat(path_text, &status) != 0) {
        int saved_error = errno;
        free(path_text);
        if (saved_error == ENOENT) return out;
        out.error = io_error_from_errno(saved_error);
        out.error_message = io_message(out.error);
        return out;
    }
    free(path_text);
    out.exists = true;
    out.is_file = S_ISREG(status.st_mode);
    out.is_dir = S_ISDIR(status.st_mode);
    out.size = out.is_file ? (int64_t)status.st_size : 0;
    out.modified_unix = (int64_t)status.st_mtime;
    return out;
}

/* Whether two paths that both exist name the same file. On Windows stat()
 * reports st_ino as 0 for every file, so dev/ino would call any two files
 * on one drive the same; the volume serial and file index are the
 * identity there. */
static bool io_same_file(const char *a, const char *b,
                         const struct stat *a_info, const struct stat *b_info) {
#ifndef _WIN32
    (void)a;
    (void)b;
    return a_info->st_dev == b_info->st_dev && a_info->st_ino == b_info->st_ino;
#else
    (void)a_info;
    (void)b_info;
    bool same = false;
    HANDLE ha = CreateFileA(a, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    HANDLE hb = CreateFileA(b, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    BY_HANDLE_FILE_INFORMATION ia, ib;
    if (ha != INVALID_HANDLE_VALUE && hb != INVALID_HANDLE_VALUE &&
        GetFileInformationByHandle(ha, &ia) && GetFileInformationByHandle(hb, &ib)) {
        same = ia.dwVolumeSerialNumber == ib.dwVolumeSerialNumber &&
               ia.nFileIndexHigh == ib.nFileIndexHigh &&
               ia.nFileIndexLow == ib.nFileIndexLow;
    }
    if (ha != INVALID_HANDLE_VALUE) CloseHandle(ha);
    if (hb != INVALID_HANDLE_VALUE) CloseHandle(hb);
    return same;
#endif
}

Iron_FileWriteResult Iron_io_copy_file(Iron_String source,
                                        Iron_String destination,
                                        bool overwrite) {
    char *source_text = NULL;
    char *destination_text = NULL;
    int source_status = io_path_copy(source, &source_text);
    int destination_status = io_path_copy(destination, &destination_text);
    if (source_status <= 0 || destination_status <= 0) {
        free(source_text);
        free(destination_text);
        return io_write_result(0,
            source_status < 0 || destination_status < 0
                ? IRON_ERR_IO_NO_MEMORY : IRON_ERR_IO_INVALID_ARGUMENT);
    }
    struct stat source_info;
    if (stat(source_text, &source_info) != 0) {
        int64_t code = io_error_from_errno(errno);
        free(source_text);
        free(destination_text);
        return io_write_result(0, code);
    }
    if (!S_ISREG(source_info.st_mode)) {
        free(source_text);
        free(destination_text);
        return io_write_result(0, S_ISDIR(source_info.st_mode)
            ? IRON_ERR_IO_IS_DIRECTORY : IRON_ERR_IO_INVALID_ARGUMENT);
    }
    struct stat destination_info;
    if (stat(destination_text, &destination_info) == 0) {
        if (io_same_file(source_text, destination_text,
                         &source_info, &destination_info)) {
            free(source_text);
            free(destination_text);
            return io_write_result(0, IRON_ERR_IO_INVALID_ARGUMENT);
        }
        if (!overwrite) {
            free(source_text);
            free(destination_text);
            return io_write_result(0, IRON_ERR_IO_ALREADY_EXISTS);
        }
    }
    FILE *input = fopen(source_text, "rb");
    if (!input) {
        int64_t code = io_error_from_errno(errno);
        free(source_text);
        free(destination_text);
        return io_write_result(0, code);
    }
    FILE *output = fopen(destination_text, overwrite ? "wb" : "wbx");
    if (!output) {
        int64_t code = io_error_from_errno(errno);
        fclose(input);
        free(source_text);
        free(destination_text);
        return io_write_result(0, code);
    }
    uint8_t buffer[64 * 1024];
    int64_t total = 0;
    int failed = 0;
    for (;;) {
        size_t received = fread(buffer, 1, sizeof(buffer), input);
        if (received == 0) {
            if (ferror(input)) failed = 1;
            break;
        }
        size_t written = 0;
        while (written < received) {
            size_t count = fwrite(buffer + written, 1, received - written, output);
            if (count == 0) { failed = 1; break; }
            written += count;
            total += (int64_t)count;
        }
        if (failed) break;
    }
    if (fflush(output) != 0) failed = 1;
    if (fclose(input) != 0) failed = 1;
    if (fclose(output) != 0) failed = 1;
    if (failed) remove(destination_text);
    free(source_text);
    free(destination_text);
    return io_write_result(total, failed ? IRON_ERR_IO_WRITE : 0);
}

Iron_FileWriteResult Iron_io_move_file(Iron_String source,
                                        Iron_String destination,
                                        bool overwrite) {
    char *source_text = NULL;
    char *destination_text = NULL;
    int source_status = io_path_copy(source, &source_text);
    int destination_status = io_path_copy(destination, &destination_text);
    if (source_status <= 0 || destination_status <= 0) {
        free(source_text);
        free(destination_text);
        return io_write_result(0,
            source_status < 0 || destination_status < 0
                ? IRON_ERR_IO_NO_MEMORY : IRON_ERR_IO_INVALID_ARGUMENT);
    }
    struct stat source_info;
    if (stat(source_text, &source_info) != 0) {
        int64_t code = io_error_from_errno(errno);
        free(source_text);
        free(destination_text);
        return io_write_result(0, code);
    }
    if (!S_ISREG(source_info.st_mode)) {
        free(source_text);
        free(destination_text);
        return io_write_result(0, IRON_ERR_IO_IS_DIRECTORY);
    }
    struct stat destination_info;
    if (stat(destination_text, &destination_info) == 0) {
        if (io_same_file(source_text, destination_text,
                         &source_info, &destination_info)) {
            free(source_text);
            free(destination_text);
            return io_write_result(0, IRON_ERR_IO_INVALID_ARGUMENT);
        }
        if (!overwrite) {
            free(source_text);
            free(destination_text);
            return io_write_result(0, IRON_ERR_IO_ALREADY_EXISTS);
        }
    }
#ifndef _WIN32
    /* POSIX rename(2) always replaces an existing destination.  For the
     * no-overwrite contract, link(2) is the portable atomic create-if-absent
     * primitive: either our source inode becomes the destination, or a racing
     * creator wins and link reports EEXIST.  Unlinking the old name completes
     * the move without opening a stat/rename race. */
    int move_result = overwrite
        ? rename(source_text, destination_text)
        : link(source_text, destination_text);
    if (move_result == 0) {
        if (!overwrite && unlink(source_text) != 0) {
            /* Both names still refer to the same inode.  Do not remove the
             * successfully committed destination: callers can safely retry
             * cleanup of the source and no data has been lost. */
            free(source_text);
            free(destination_text);
            return io_write_result((int64_t)source_info.st_size,
                                   IRON_ERR_IO_WRITE);
        }
        int64_t size = (int64_t)source_info.st_size;
        free(source_text);
        free(destination_text);
        return io_write_result(size, 0);
    }
    int rename_error = errno;
#ifdef EXDEV
    if (rename_error == EXDEV) {
        if (overwrite) {
            free(source_text);
            free(destination_text);
            Iron_FileWriteResult copied = Iron_io_copy_file(
                source, destination, true);
            if (copied.error != 0) return copied;
            char *source_again = NULL;
            if (io_path_copy(source, &source_again) <= 0 ||
                remove(source_again) != 0) {
                free(source_again);
                return io_write_result(copied.bytes, IRON_ERR_IO_WRITE);
            }
            free(source_again);
            return copied;
        }

        /* A cross-device no-overwrite move must not expose a partial target or
         * let a racing creator be replaced.  Copy to an exclusive temporary
         * file in the destination filesystem, flush it, then atomically link
         * that complete file into the requested name. */
        size_t destination_length = strlen(destination_text);
        static const char suffix[] = ".iron-move-XXXXXX";
        char *temporary = (char *)malloc(destination_length + sizeof(suffix));
        if (!temporary) {
            free(source_text);
            free(destination_text);
            return io_write_result(0, IRON_ERR_IO_NO_MEMORY);
        }
        memcpy(temporary, destination_text, destination_length);
        memcpy(temporary + destination_length, suffix, sizeof(suffix));
        int temporary_fd = mkstemp(temporary);
        if (temporary_fd < 0) {
            int temporary_error = errno;
            free(temporary);
            free(source_text);
            free(destination_text);
            return io_write_result(0, io_error_from_errno(temporary_error));
        }
        (void)fchmod(temporary_fd, source_info.st_mode & 0777);
        FILE *input = fopen(source_text, "rb");
        FILE *output = fdopen(temporary_fd, "wb");
        int64_t total = 0;
        int failed = !input || !output;
        if (!output) close(temporary_fd);
        uint8_t buffer[64 * 1024];
        while (!failed) {
            size_t received = fread(buffer, 1, sizeof(buffer), input);
            if (received == 0) {
                if (ferror(input)) failed = 1;
                break;
            }
            size_t written = 0;
            while (written < received) {
                size_t count = fwrite(buffer + written, 1,
                                      received - written, output);
                if (count == 0) { failed = 1; break; }
                written += count;
                total += (int64_t)count;
            }
        }
        if (output && (fflush(output) != 0 || fsync(fileno(output)) != 0)) {
            failed = 1;
        }
        if (input && fclose(input) != 0) failed = 1;
        if (output && fclose(output) != 0) failed = 1;
        if (failed) {
            unlink(temporary);
            free(temporary);
            free(source_text);
            free(destination_text);
            return io_write_result(total, IRON_ERR_IO_WRITE);
        }
        if (link(temporary, destination_text) != 0) {
            int commit_error = errno;
            unlink(temporary);
            free(temporary);
            free(source_text);
            free(destination_text);
            return io_write_result(0, io_error_from_errno(commit_error));
        }
        unlink(temporary);
        free(temporary);
        if (unlink(source_text) != 0) {
            free(source_text);
            free(destination_text);
            return io_write_result(total, IRON_ERR_IO_WRITE);
        }
        free(source_text);
        free(destination_text);
        return io_write_result(total, 0);
    }
#endif
    free(source_text);
    free(destination_text);
    return io_write_result(0, io_error_from_errno(rename_error));
#else
    /* MoveFileEx without MOVEFILE_REPLACE_EXISTING is Windows' atomic
     * no-replace primitive.  MOVEFILE_COPY_ALLOWED supplies the cross-volume
     * fallback while retaining the same destination-exists contract. */
    DWORD flags = MOVEFILE_COPY_ALLOWED;
    if (overwrite) flags |= MOVEFILE_REPLACE_EXISTING;
    if (MoveFileExA(source_text, destination_text, flags)) {
        int64_t size = (int64_t)source_info.st_size;
        free(source_text);
        free(destination_text);
        return io_write_result(size, 0);
    }
    DWORD move_error = GetLastError();
    free(source_text);
    free(destination_text);
    if (move_error == ERROR_ALREADY_EXISTS || move_error == ERROR_FILE_EXISTS) {
        return io_write_result(0, IRON_ERR_IO_ALREADY_EXISTS);
    }
    if (move_error == ERROR_ACCESS_DENIED) {
        return io_write_result(0, IRON_ERR_IO_PERMISSION);
    }
    return io_write_result(0, IRON_ERR_IO_OTHER);
#endif
}
