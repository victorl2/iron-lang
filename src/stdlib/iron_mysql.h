#ifndef IRON_MYSQL_H
#define IRON_MYSQL_H

/* iron_mysql.h -- native MySQL/MariaDB client behind stdlib/mysql.iron.
 *
 * The struct layouts below mirror the objects declared in
 * src/stdlib/mysql.iron field for field (Int -> int64_t, UInt -> uint64_t,
 * Float -> double, Bool -> bool, String -> Iron_String, an enum without
 * payloads -> a C enum). Generated C never includes this header: it
 * declares the same types itself, and the two only have to agree on
 * layout. Unit tests include it to call the entry points directly.
 *
 * Handles are pointers to native objects passed through Iron as Int. 0 is
 * accepted everywhere and behaves as a closed object.
 */

#include <stdbool.h>
#include <stdint.h>

#include "runtime/iron_runtime.h"

typedef struct Iron_MysqlError {
    int64_t code;
    Iron_String sqlstate;
    Iron_String message;
    bool server;
} Iron_MysqlError;

enum {
    IRON_MYSQL_TLS_DISABLED = 0,
    IRON_MYSQL_TLS_PREFERRED = 1,
    IRON_MYSQL_TLS_REQUIRED = 2,
    IRON_MYSQL_TLS_VERIFY_CA = 3,
    IRON_MYSQL_TLS_VERIFY_IDENTITY = 4
};

typedef struct Iron_MysqlConfig {
    Iron_String host;
    int64_t port;
    Iron_String user;
    Iron_String password;
    Iron_String database;
    int tls;                        /* enum MysqlTls */
    Iron_String ca_file;
    Iron_String server_public_key;
    bool allow_public_key_retrieval;
    bool allow_cleartext_password;
    Iron_String collation;
    int64_t connect_timeout;
    int64_t timeout;
    bool multi_statements;
    int64_t max_allowed_packet;
    Iron_String init_command;
} Iron_MysqlConfig;

typedef struct Iron_MysqlDateTime {
    int64_t year;
    int64_t month;
    int64_t day;
    int64_t hour;
    int64_t minute;
    int64_t second;
    int64_t microsecond;
} Iron_MysqlDateTime;

typedef struct Iron_MysqlTime {
    bool negative;
    int64_t hours;
    int64_t minutes;
    int64_t seconds;
    int64_t microsecond;
} Iron_MysqlTime;

/* enum MysqlValue: a tag followed by a union of one-field payload structs,
 * the layout the compiler gives every enum with payloads. */
typedef enum {
    Iron_MysqlValue_TAG_Null = 0,
    Iron_MysqlValue_TAG_Int = 1,
    Iron_MysqlValue_TAG_UInt = 2,
    Iron_MysqlValue_TAG_Float = 3,
    Iron_MysqlValue_TAG_Decimal = 4,
    Iron_MysqlValue_TAG_Text = 5,
    Iron_MysqlValue_TAG_Bytes = 6,
    Iron_MysqlValue_TAG_Bool = 7,
    Iron_MysqlValue_TAG_Date = 8,
    Iron_MysqlValue_TAG_DateTime = 9,
    Iron_MysqlValue_TAG_Time = 10
} Iron_MysqlValue_Tag;

typedef union {
    char _dummy;
    struct { int64_t _0; } Int;
    struct { uint64_t _0; } UInt;
    struct { double _0; } Float;
    struct { Iron_String _0; } Decimal;
    struct { Iron_String _0; } Text;
    struct { Iron_String _0; } Bytes;
    struct { bool _0; } Bool;
    struct { Iron_MysqlDateTime _0; } Date;
    struct { Iron_MysqlDateTime _0; } DateTime;
    struct { Iron_MysqlTime _0; } Time;
} Iron_MysqlValue_data_t;

typedef struct Iron_MysqlValue {
    Iron_MysqlValue_Tag tag;
    Iron_MysqlValue_data_t data;
} Iron_MysqlValue;

typedef struct Iron_MysqlValueList {
    Iron_MysqlValue *items;
    int64_t count;
    int64_t capacity;
} Iron_MysqlValueList;

typedef struct Iron_MysqlColumn {
    Iron_String name;
    Iron_String org_name;
    Iron_String table;
    Iron_String org_table;
    Iron_String schema;
    int64_t type;
    Iron_String type_name;
    int64_t flags;
    int64_t decimals;
    int64_t length;
    int64_t charset;
} Iron_MysqlColumn;

typedef struct Iron_MysqlPoolOptions {
    int64_t min;
    int64_t max;
    int64_t idle_timeout;
    int64_t acquire_timeout;
    int64_t health_check;
    int64_t max_lifetime;
    bool reset_on_release;
} Iron_MysqlPoolOptions;

/* Tuple results: (T, MysqlError). */
typedef struct { Iron_MysqlConfig v0; Iron_MysqlError v1; } Iron_MysqlConfigResult;
typedef struct { int64_t v0; Iron_MysqlError v1; } Iron_MysqlIntResult;
typedef struct { uint64_t v0; Iron_MysqlError v1; } Iron_MysqlUIntResult;
typedef struct { double v0; Iron_MysqlError v1; } Iron_MysqlFloatResult;
typedef struct { Iron_String v0; Iron_MysqlError v1; } Iron_MysqlStringResult;
typedef struct { bool v0; Iron_MysqlError v1; } Iron_MysqlBoolResult;
typedef struct { Iron_MysqlDateTime v0; Iron_MysqlError v1; } Iron_MysqlDateTimeResult;
typedef struct { Iron_MysqlTime v0; Iron_MysqlError v1; } Iron_MysqlTimeResult;

/* Connections. */
int64_t Iron_mysqlnative_connect(Iron_MysqlConfig config);
Iron_MysqlConfigResult Iron_mysqlnative_parse_url(Iron_String url);
Iron_MysqlError Iron_mysqlnative_conn_error(int64_t h);
void Iron_mysqlnative_conn_free(int64_t h);
void Iron_mysqlnative_conn_close(int64_t h);
void Iron_mysqlnative_conn_set(int64_t h, int64_t what, int64_t value);
int64_t Iron_mysqlnative_conn_info(int64_t h, int64_t what);
Iron_String Iron_mysqlnative_server_version(int64_t h);
Iron_String Iron_mysqlnative_auth_plugin(int64_t h);
int64_t Iron_mysqlnative_query(int64_t h, Iron_String sql);
int64_t Iron_mysqlnative_exec(int64_t h, Iron_String sql, Iron_MysqlValueList params);
Iron_MysqlError Iron_mysqlnative_run(int64_t h, Iron_String sql);
Iron_MysqlError Iron_mysqlnative_command(int64_t h, int64_t what, Iron_String arg);
Iron_String Iron_mysqlnative_escape(int64_t h, Iron_String s);
Iron_String Iron_mysqlnative_quote_identifier(Iron_String name);

/* Prepared statements. */
int64_t Iron_mysqlnative_prepare(int64_t h, Iron_String sql);
Iron_MysqlError Iron_mysqlnative_stmt_error(int64_t h);
int64_t Iron_mysqlnative_stmt_exec(int64_t h, Iron_MysqlValueList params);
Iron_MysqlError Iron_mysqlnative_stmt_reset(int64_t h);
void Iron_mysqlnative_stmt_close(int64_t h);
void Iron_mysqlnative_stmt_free(int64_t h);
int64_t Iron_mysqlnative_stmt_info(int64_t h, int64_t what);

/* Results. */
Iron_MysqlError Iron_mysqlnative_res_error(int64_t h);
void Iron_mysqlnative_res_free(int64_t h);
int64_t Iron_mysqlnative_res_info(int64_t h, int64_t what);
Iron_String Iron_mysqlnative_res_info_text(int64_t h);
bool Iron_mysqlnative_res_next(int64_t h);
Iron_MysqlColumn Iron_mysqlnative_res_column(int64_t h, int64_t index);
int64_t Iron_mysqlnative_res_col(int64_t h, Iron_String name);
bool Iron_mysqlnative_res_is_null(int64_t h, int64_t row, int64_t col);
Iron_MysqlIntResult Iron_mysqlnative_res_int(int64_t h, int64_t row, int64_t col);
Iron_MysqlUIntResult Iron_mysqlnative_res_uint(int64_t h, int64_t row, int64_t col);
Iron_MysqlFloatResult Iron_mysqlnative_res_float(int64_t h, int64_t row, int64_t col);
Iron_MysqlStringResult Iron_mysqlnative_res_text(int64_t h, int64_t row, int64_t col);
Iron_MysqlBoolResult Iron_mysqlnative_res_bool(int64_t h, int64_t row, int64_t col);
Iron_MysqlDateTimeResult Iron_mysqlnative_res_datetime(int64_t h, int64_t row, int64_t col);
Iron_MysqlTimeResult Iron_mysqlnative_res_time(int64_t h, int64_t row, int64_t col);
Iron_MysqlValue Iron_mysqlnative_res_value(int64_t h, int64_t row, int64_t col);
Iron_String Iron_mysqlnative_format_datetime(Iron_MysqlDateTime v);
Iron_String Iron_mysqlnative_format_date(Iron_MysqlDateTime v);
Iron_String Iron_mysqlnative_format_time(Iron_MysqlTime v);

/* Pools. */
int64_t Iron_mysqlnative_pool_open(Iron_MysqlConfig config, Iron_MysqlPoolOptions options);
Iron_MysqlError Iron_mysqlnative_pool_error(int64_t h);
int64_t Iron_mysqlnative_pool_acquire(int64_t h);
int64_t Iron_mysqlnative_pool_query(int64_t h, Iron_String sql);
int64_t Iron_mysqlnative_pool_exec(int64_t h, Iron_String sql, Iron_MysqlValueList params);
void Iron_mysqlnative_pool_close(int64_t h);
void Iron_mysqlnative_pool_free(int64_t h);
int64_t Iron_mysqlnative_pool_info(int64_t h, int64_t what);

/* False when the module was compiled without OpenSSL: connections then
 * fail with IRON_ERR_MYSQL_UNAVAILABLE (the auth hashing and TLS come from
 * libcrypto and libssl). */
bool iron_mysql_available(void);

/* Protocol helpers exposed for the unit tests (no server needed). */
void iron_mysql_scramble_native(const uint8_t *scramble, size_t scramble_len,
                                const char *password, size_t password_len,
                                uint8_t out[20]);
void iron_mysql_scramble_sha256(const uint8_t *scramble, size_t scramble_len,
                                const char *password, size_t password_len,
                                uint8_t out[32]);
/* Escape into out (capacity >= 2*len+1); returns the escaped length. */
size_t iron_mysql_escape_bytes(char *out, const char *in, size_t len,
                               bool no_backslash_escapes);

/* The debugger's summary of a result (kind 0), connection (1) or pool (2)
 * handle (src/debug formatters). */
const char *iron_mysql_debug_text(int64_t h, int64_t kind);

#endif /* IRON_MYSQL_H */
