#!/usr/bin/env python3
"""Print markdown tables from bench/http SAMPLE lines."""
import argparse
import statistics

LABELS = {
    "small": "Small GET",
    "browser": "Browser-like request",
    "chunked": "1 MiB chunked upload",
    "pipeline": "128-request pipeline",
    "json4k": "4 KiB response",
    "large": "1 MiB response",
    "minimal": "Minimal persistent server",
}

SERIALIZE_LABELS = {
    "small": "2-byte response",
    "json4k": "4 KiB response",
    "large": "1 MiB response",
}


def load(path):
    rows = {}
    for line in open(path, encoding="utf-8"):
        if not line.startswith("SAMPLE "):
            continue
        _, impl, mode, workload, byte_count, messages, elapsed, checksum = line.split()
        key = (mode, workload, impl)
        rows.setdefault(key, []).append(
            (int(byte_count), int(messages), int(elapsed), int(checksum)))
    return rows


def metric(samples, kind):
    values = []
    for byte_count, messages, elapsed, _ in samples:
        if kind == "gbps":
            values.append(byte_count / elapsed)
        else:
            values.append(messages * 1e9 / elapsed)
    median = statistics.median(values)
    mad = statistics.median(abs(value - median) for value in values)
    return median, (100 * mad / median if median else 0)


def value(samples, kind, scale=1):
    median, mad = metric(samples, kind)
    return "{:.3f} (+/- {:.1f}%)".format(median / scale, mad)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("results")
    args = parser.parse_args()
    rows = load(args.results)

    print("### Parse only\n")
    print("| Workload | Iron GB/s | Iron Mmsg/s | pico GB/s | pico Mmsg/s | Iron/pico |")
    print("|---|---:|---:|---:|---:|---:|")
    for workload in ("small", "browser", "chunked", "pipeline"):
        iron = rows[("parse", workload, "iron")]
        pico = rows[("parse", workload, "pico")]
        iron_gb, _ = metric(iron, "gbps")
        pico_gb, _ = metric(pico, "gbps")
        print("| {} | {} | {} | {} | {} | {:.3f} |".format(
            LABELS[workload], value(iron, "gbps"), value(iron, "mps", 1e6),
            value(pico, "gbps"), value(pico, "mps", 1e6), iron_gb / pico_gb))

    print("\n### Response serialization\n")
    print("| Workload | Iron GB/s | Iron Mmsg/s | C GB/s | C Mmsg/s | Iron/C |")
    print("|---|---:|---:|---:|---:|---:|")
    for workload in ("small", "json4k", "large"):
        iron = rows[("serialize", workload, "iron")]
        reference = rows[("serialize", workload, "c")]
        iron_gb, _ = metric(iron, "gbps")
        ref_gb, _ = metric(reference, "gbps")
        print("| {} | {} | {} | {} | {} | {:.3f} |".format(
            SERIALIZE_LABELS[workload], value(iron, "gbps"), value(iron, "mps", 1e6),
            value(reference, "gbps"), value(reference, "mps", 1e6),
            iron_gb / ref_gb))

    print("\n### End to end\n")
    print("| Workload | Iron requests/s | pico requests/s | Iron/pico |")
    print("|---|---:|---:|---:|")
    iron = rows[("e2e", "minimal", "iron")]
    pico = rows[("e2e", "minimal", "pico")]
    iron_rps, _ = metric(iron, "mps")
    pico_rps, _ = metric(pico, "mps")
    print("| {} | {} | {} | {:.3f} |".format(
        LABELS["minimal"], value(iron, "mps"), value(pico, "mps"),
        iron_rps / pico_rps))


if __name__ == "__main__":
    main()
