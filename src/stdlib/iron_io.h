#ifndef IRON_IO_H
#define IRON_IO_H

#include <stdint.h>
#include <stdbool.h>
#include "runtime/iron_runtime.h"

/* ── Result types ────────────────────────────────────────────────────────── */
typedef struct { Iron_String v0; Iron_Error v1; } Iron_Result_String_Error;
typedef struct { bool       v0; Iron_Error v1; } Iron_Result_bool_Error;

/* ── File I/O ────────────────────────────────────────────────────────────── */
Iron_Result_String_Error Iron_io_read_file_result(Iron_String path);
Iron_Error               Iron_io_write_file(Iron_String path, Iron_String content);
Iron_Result_String_Error Iron_io_read_bytes_result(Iron_String path);
Iron_Error               Iron_io_write_bytes(Iron_String path, const uint8_t *data, size_t len);

/* ── File system ─────────────────────────────────────────────────────────── */
bool       Iron_io_file_exists(Iron_String path);
Iron_Error Iron_io_create_dir(Iron_String path);
Iron_Error Iron_io_delete_file(Iron_String path);

/* list_files returns a newline-separated Iron_String of filenames */
Iron_Result_String_Error Iron_io_list_files_result(Iron_String dir_path);

/* Wrappers that return just the value -- used by auto-static dispatch from Iron source */
static inline Iron_String Iron_io_read_file(Iron_String path) {
    return Iron_io_read_file_result(path).v0;
}
static inline Iron_String Iron_io_list_files(Iron_String dir_path) {
    return Iron_io_list_files_result(dir_path).v0;
}
static inline Iron_String Iron_io_read_bytes(Iron_String path) {
    return Iron_io_read_bytes_result(path).v0;
}

/* ── Phase 39 additions ──────────────────────────────────────────────────── */
Iron_String            Iron_io_read_line(void);
Iron_Error             Iron_io_append_file(Iron_String path, Iron_String content);
Iron_String            Iron_io_basename(Iron_String path);
Iron_String            Iron_io_dirname(Iron_String path);
Iron_String            Iron_io_join_path(Iron_String a, Iron_String b);
Iron_String            Iron_io_extension(Iron_String path);
bool                   Iron_io_is_dir(Iron_String path);
Iron_List_Iron_String  Iron_io_read_lines(Iron_String path);

#endif /* IRON_IO_H */
