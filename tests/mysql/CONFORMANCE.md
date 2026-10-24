# MySQL protocol conformance matrix

`test_mysql_conformance` runs without a server. It sends recorded and hand-built bytes through an in-process transport into the production C codec. The transport can return one byte per read or any larger chunk and captures client writes. The suite follows the packet and field layouts in the MySQL Client/Server Protocol documentation, including the basic packet format, connection phase packets, generic response packets, text result sets, binary result sets and prepared statements.

Coverage:

| Area | Cases |
|---|---|
| Framing | 3-byte payload length, sequence wrap, wrong sequence, truncated header and body, configured packet limit, 0xffffff continuation, continuation payload, required empty trailing packet, a split after every byte position |
| Length-encoded data | Integer markers 0 through 250, 0xfc, 0xfd, 0xfe and 0xfb NULL; string payloads in the one-byte, 0xfc, 0xfd and 0xfe classes; truncated and reserved markers |
| Connection | HandshakeV10, handshake response fields, mysql_native_password auth switch, caching_sha2_password fast path, RSA public-key full path, insecure full-path rejection, every truncated handshake prefix |
| Generic packets | OK summaries, protocol 4.1 ERR and SQLSTATE, legacy EOF, CLIENT_DEPRECATE_EOF OK terminator, multi-result status |
| Metadata and rows | Complete ColumnDefinition41 metadata; text and binary rows for every integer width signed and unsigned, DECIMAL, FLOAT, DOUBLE, DATE, negative fractional TIME, DATETIME, TIMESTAMP, YEAR, BIT, JSON, ENUM, SET, all BLOB families and text/string families; NULL bitmap positions crossing bytes |
| Prepared statements | COM_STMT_PREPARE and metadata, every MysqlValue parameter form and type flag, COM_STMT_EXECUTE, binary result, queued COM_STMT_CLOSE |
| Streaming | Complete representative result and authentication exchanges at every read chunk size; every truncated prefix of a representative result and handshake |

Malformed packet policy:

| Input | Required result |
|---|---|
| Wrong packet sequence | `IRON_ERR_MYSQL_PROTOCOL` |
| Truncated packet header or payload | network closed error, never a successful partial result |
| Payload over configured limit | `IRON_ERR_MYSQL_PACKET_TOO_LARGE` |
| Truncated or reserved length-encoded integer | `IRON_ERR_MYSQL_PROTOCOL` |
| NULL length marker in a required string or OK integer | `IRON_ERR_MYSQL_PROTOCOL` |
| Result header suffix, zero or excessive column count | `IRON_ERR_MYSQL_PROTOCOL` |
| Column fixed-field length other than 0x0c, missing filler or suffix | `IRON_ERR_MYSQL_PROTOCOL` |
| Legacy EOF shorter or longer than five bytes | `IRON_ERR_MYSQL_PROTOCOL` |
| ERR without its code or protocol 4.1 SQLSTATE | `IRON_ERR_MYSQL_PROTOCOL` |
| Text row missing, overlong or with extra cells | `IRON_ERR_MYSQL_PROTOCOL` |
| Binary row with a bad header, bitmap, value length or temporal length | `IRON_ERR_MYSQL_PROTOCOL` |
| COM_STMT_PREPARE_OK not exactly 12 bytes | `IRON_ERR_MYSQL_PROTOCOL` |
| Authentication switch without a terminated plugin name, unknown plugin or unsupported old-password request | authentication or protocol error, never success |

The live `test_mysql_differential` case is complementary. When `IRON_MYSQL_URL` and a `mysql` or `mariadb` command line client are available, it generates a 30-column table and compares the Iron client binary-protocol result with independently normalized reference-client output. It exits 77 when either dependency is unavailable, which ctest reports as skipped.
