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
