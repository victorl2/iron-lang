#include "iron_io.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>  /* WINDOWS-TODO: no dirent.h on Windows — use FindFirstFile/FindNextFile/FindClose from <windows.h> */
#include <errno.h>

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

Iron_Error Iron_io_write_bytes(Iron_String path, const uint8_t *data, size_t len) {
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
    /* Remove trailing empty string if file ended with \n (common case) */
    if (lines.count > 0) {
        Iron_String last = lines.items[lines.count - 1];
        if (iron_string_byte_len(&last) == 0) {
            lines.count--;
        }
    }
    return lines;
}
