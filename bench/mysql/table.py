#!/usr/bin/env python3
import statistics
import sys

rows = []
for line in sys.stdin:
    if not line.startswith("RESULT "):
        continue
    _, operation, case, byte_count, row_count, median_ns, mad_ns = line.split()
    b, r, ns, mad = map(int, (byte_count, row_count, median_ns, mad_ns))
    mbps = b / (ns / 1e9) / 1e6
    rps = r / (ns / 1e9)
    spread = mad / ns * 100 if ns else 0
    rows.append((operation, case, mbps, rps, spread))

print("| Operation | Fixture | MB/s | rows or params/s | MAD | MariaDB Connector/C |")
print("|---|---|---:|---:|---:|---|")
for operation, case, mbps, rps, spread in rows:
    print(f"| {operation} | {case} | {mbps:.1f} | {rps:,.0f} | {spread:.2f}% | N/A (no memory codec API) |")
