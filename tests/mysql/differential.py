#!/usr/bin/env python3
"""Live differential check against mysql or mariadb command line client."""
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import urllib.parse

ROOT = pathlib.Path(__file__).resolve().parents[2]
COLUMNS = [
    "ti", "tu", "si", "su", "mi", "mu", "i", "iu", "bi", "bu", "dc", "f", "d",
    "dt", "tm", "dtm", "ts", "yr", "bt", "js", "en", "st", "tb", "bl", "mb", "lb",
    "tt", "tx", "mt", "lt",
]
DDL = """
DROP TABLE IF EXISTS iron_differential;
CREATE TABLE iron_differential (
 id INT PRIMARY KEY, ti TINYINT, tu TINYINT UNSIGNED, si SMALLINT, su SMALLINT UNSIGNED,
 mi MEDIUMINT, mu MEDIUMINT UNSIGNED, i INT, iu INT UNSIGNED, bi BIGINT, bu BIGINT UNSIGNED,
 dc DECIMAL(30,6), f FLOAT, d DOUBLE, dt DATE, tm TIME(6), dtm DATETIME(6), ts TIMESTAMP(3),
 yr YEAR, bt BIT(17), js JSON, en ENUM('red','green','blue'), st SET('x','y','z'),
 tb TINYBLOB, bl BLOB, mb MEDIUMBLOB, lb LONGBLOB,
 tt TINYTEXT, tx TEXT, mt MEDIUMTEXT, lt LONGTEXT
) CHARACTER SET utf8mb4;
INSERT INTO iron_differential VALUES
(1,-128,255,-32768,65535,-8388608,16777215,-2147483648,4294967295,
 -9223372036854775808,18446744073709551615,-12345678901234567890.125000,-1.25,
 3.141592653589793,'2024-02-29','-123:04:05.678901','2024-02-29 13:45:30.123456',
 '2038-01-19 03:14:07.500',2155,b'10100101101001011','{"k":[1,2]}','green','x,z',
 X'00FF41',X'000102FF',X'6D656469756D',X'6C6F6E67','tiny','text','medium','long'),
(2,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,
 NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL);
"""

def main() -> int:
    if len(sys.argv) != 2:
        print("usage: differential.py <ironc>", file=sys.stderr)
        return 2
    ironc = str(pathlib.Path(sys.argv[1]).resolve())
    url = os.environ.get("IRON_MYSQL_URL", "")
    client = shutil.which("mysql") or shutil.which("mariadb")
    if not url or not client:
        print("SKIP: IRON_MYSQL_URL or mysql/mariadb command line client is unavailable")
        return 77
    parsed = urllib.parse.urlsplit(url)
    database = parsed.path.lstrip("/")
    args = [client, "--protocol=TCP", "--batch", "--raw", "--skip-column-names",
            "--host", parsed.hostname or "127.0.0.1", "--port", str(parsed.port or 3306),
            "--user", urllib.parse.unquote(parsed.username or "root"), database]
    env = os.environ.copy()
    env["MYSQL_PWD"] = urllib.parse.unquote(parsed.password or "")
    probe = subprocess.run(args + ["-e", "SELECT 1"], env=env, text=True,
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if probe.returncode:
        print("SKIP: MySQL server is not reachable by the reference client")
        return 77
    subprocess.run(args + ["-e", DDL], env=env, check=True)
    try:
        binary_columns = {"tb", "bl", "mb", "lb"}
        ref_expr = ",".join(
            f"IF({c} IS NULL,'NULL',LOWER(HEX({c})))" if c in binary_columns
            else f"IF({c} IS NULL,'NULL',LOWER(HEX(CAST({c} AS CHAR))))"
            for c in COLUMNS
        )
        reference = subprocess.run(args + ["-e", f"SELECT {ref_expr} FROM iron_differential ORDER BY id"],
                                   env=env, check=True, text=True, stdout=subprocess.PIPE).stdout
        with tempfile.TemporaryDirectory(prefix="iron-mysql-diff-") as work:
            binary = pathlib.Path(work) / "differential"
            subprocess.run([ironc, "build", str(ROOT / "tests/mysql/support/differential.iron"),
                            "-o", str(binary)], cwd=work, check=True)
            actual_run = subprocess.run([str(binary)], env=env, text=True,
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if actual_run.returncode or actual_run.stdout.startswith("ERROR"):
            print(actual_run.stdout, end="", file=sys.stderr)
            print(actual_run.stderr, end="", file=sys.stderr)
            return 1
        if actual_run.stdout != reference:
            print("reference client and Iron client differ", file=sys.stderr)
            print("reference:\n" + reference, file=sys.stderr)
            print("Iron:\n" + actual_run.stdout, file=sys.stderr)
            return 1
        print(f"PASS differential: {len(COLUMNS)} column types, 2 rows")
        return 0
    finally:
        subprocess.run(args + ["-e", "DROP TABLE IF EXISTS iron_differential"], env=env,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

if __name__ == "__main__":
    raise SystemExit(main())
