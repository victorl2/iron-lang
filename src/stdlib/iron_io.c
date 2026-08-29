#include "iron_io.h"
#include "runtime/iron_errors.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>  /* WINDOWS-TODO: no dirent.h on Windows — use FindFirstFile/FindNextFile/FindClose from <windows.h> */
#include <errno.h>
#ifdef _WIN32
#include <windows.h>
#endif
#ifndef _WIN32
#include <unistd.h>
#endif

/* ── File I/O ────────────────────────────────────────────────────────────── */

Iron_Result_String_Error Iron_io_read_file_result(Iron_String path) {
    const char *p = iron_string_cstr(&path);
    FILE *f = fopen(p, "rb");
    if (!f) {
        Iron_String empty = iron_string_from_cstr("", 0);
        return (Iron_Result_String_Error){ empty, iron_error_new(1, "file not found") };
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        Iron_String empty = iron_string_from_cstr("", 0);
        return (Iron_Result_String_Error){ empty, iron_error_new(2, "seek failed") };
    }

    long file_size = ftell(f);
    if (file_size < 0) {
        fclose(f);
        Iron_String empty = iron_string_from_cstr("", 0);
        return (Iron_Result_String_Error){ empty, iron_error_new(3, "tell failed") };
    }

    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        Iron_String empty = iron_string_from_cstr("", 0);
        return (Iron_Result_String_Error){ empty, iron_error_new(2, "seek failed") };
    }

    size_t size = (size_t)file_size;
    char *buf = (char *)malloc(size + 1);
    if (!buf) {
        fclose(f);
        Iron_String empty = iron_string_from_cstr("", 0);
        return (Iron_Result_String_Error){ empty, iron_error_new(4, "out of memory") };
    }

    size_t read_count = fread(buf, 1, size, f);
    fclose(f);
    buf[read_count] = '\0';

    Iron_String result = iron_string_from_cstr(buf, read_count);
    free(buf);

    return (Iron_Result_String_Error){ result, iron_error_none() };
}

Iron_Error Iron_io_write_file(Iron_String path, Iron_String content) {
    const char *p = iron_string_cstr(&path);
    FILE *f = fopen(p, "wb");
    if (!f) {
        return iron_error_new(1, "could not open file for writing");
    }

    const char *data = iron_string_cstr(&content);
    size_t len = iron_string_byte_len(&content);
    size_t written = fwrite(data, 1, len, f);
    fclose(f);

    if (written != len) {
        return iron_error_new(5, "write incomplete");
    }
    return iron_error_none();
}

Iron_Result_String_Error Iron_io_read_bytes_result(Iron_String path) {
    /* Same as read_file — binary mode is already used */
    return Iron_io_read_file_result(path);
}

Iron_Error Iron_io_write_bytes_raw(Iron_String path, const uint8_t *data, size_t len) {
    const char *p = iron_string_cstr(&path);
    FILE *f = fopen(p, "wb");
    if (!f) {
        return iron_error_new(1, "could not open file for writing");
    }

    size_t written = fwrite(data, 1, len, f);
    fclose(f);

    if (written != len) {
        return iron_error_new(5, "write incomplete");
    }
    return iron_error_none();
}

/* ── File system ─────────────────────────────────────────────────────────── */

bool Iron_io_file_exists(Iron_String path) {
    const char *p = iron_string_cstr(&path);
    struct stat st;
    return stat(p, &st) == 0;
}

Iron_Error Iron_io_create_dir(Iron_String path) {
    const char *p = iron_string_cstr(&path);
    /* WINDOWS-TODO: mkdir(path, mode) is POSIX. On Windows use _mkdir(path) from <direct.h> (no mode arg). */
    int result = mkdir(p, 0755);
    if (result != 0 && errno != EEXIST) {
        return iron_error_new(6, "could not create directory");
    }
    return iron_error_none();
}

Iron_Error Iron_io_delete_file(Iron_String path) {
    const char *p = iron_string_cstr(&path);
    if (remove(p) != 0) {
        return iron_error_new(7, "could not delete file");
    }
    return iron_error_none();
}

Iron_Result_String_Error Iron_io_list_files_result(Iron_String dir_path) {
    const char *p = iron_string_cstr(&dir_path);
    /* WINDOWS-TODO: opendir/readdir/closedir are POSIX. On Windows replace this block with
     * FindFirstFileA/FindNextFileA/FindClose from <windows.h>. WIN32_FIND_DATAA carries
     * the entry name in cFileName. */
    DIR *dir = opendir(p);
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

Iron_String Iron_io_read_line(void) {
    char buf[4096];
    if (!fgets(buf, sizeof(buf), stdin)) {
        return iron_string_from_cstr("", 0);  /* EOF — return empty string */
    }
    size_t len = strlen(buf);
    /* Strip trailing CR and LF */
    if (len > 0 && buf[len - 1] == '\n') { buf[--len] = '\0'; }
    if (len > 0 && buf[len - 1] == '\r') { buf[--len] = '\0'; }
    return iron_string_from_cstr(buf, len);
}

Iron_Error Iron_io_append_file(Iron_String path, Iron_String content) {
    const char *p = iron_string_cstr(&path);
    FILE *f = fopen(p, "ab");
    if (!f) return iron_error_new(1, "could not open file for appending");
    const char *data = iron_string_cstr(&content);
    size_t len = iron_string_byte_len(&content);
    size_t written = fwrite(data, 1, len, f);
    fclose(f);
    if (written != len) return iron_error_new(5, "write incomplete");
    return iron_error_none();
}

Iron_String Iron_io_basename(Iron_String path) {
    const char *p = iron_string_cstr(&path);
    const char *last = strrchr(p, '/');
    const char *base = last ? last + 1 : p;
    return iron_string_from_cstr(base, strlen(base));
}

Iron_String Iron_io_dirname(Iron_String path) {
    const char *p = iron_string_cstr(&path);
    const char *last = strrchr(p, '/');
    if (!last) return iron_string_from_cstr(".", 1);
    if (last == p) return iron_string_from_cstr("/", 1);
    size_t len = (size_t)(last - p);
    return iron_string_from_cstr(p, len);
}

Iron_String Iron_io_join_path(Iron_String a, Iron_String b) {
    /* Always forward slash — works on Windows too (locked decision) */
    const char *ap = iron_string_cstr(&a);
    const char *bp = iron_string_cstr(&b);
    size_t alen = iron_string_byte_len(&a);
    /* Strip trailing slash(es) from a */
    while (alen > 0 && ap[alen - 1] == '/') alen--;
    size_t blen = strlen(bp);
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
    const char *last_slash = strrchr(p, '/');
    const char *base = last_slash ? last_slash + 1 : p;
    const char *dot = strrchr(base, '.');
    /* No dot, or dot is the first char of filename (e.g. ".gitignore") */
    if (!dot || dot == base) return iron_string_from_cstr("", 0);
    /* Skip the dot — return extension only */
    const char *ext = dot + 1;
    return iron_string_from_cstr(ext, strlen(ext));
}

bool Iron_io_is_dir(Iron_String path) {
    const char *p = iron_string_cstr(&path);
    struct stat st;
    if (stat(p, &st) != 0) return false;
    return S_ISDIR(st.st_mode);
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
    iron_string_release(&result.data);
    iron_string_release(&result.error_message);
}

void Iron_filewriteresult_release(Iron_FileWriteResult result) {
    iron_string_release(&result.error_message);
}

void Iron_fileinfo_release(Iron_FileInfo info) {
    iron_string_release(&info.error_message);
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
        if (source_info.st_dev == destination_info.st_dev &&
            source_info.st_ino == destination_info.st_ino) {
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
        if (source_info.st_dev == destination_info.st_dev &&
            source_info.st_ino == destination_info.st_ino) {
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
