#!/usr/bin/env python3
"""Compare canonical Iron HTTP parses with picohttpparser on generated input."""
import argparse
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))


def run(command, batch):
    completed = subprocess.run([command[0], command[1], batch],
                               text=True, capture_output=True)
    if completed.returncode != 0:
        raise RuntimeError("{} failed with {}:\n{}".format(
            " ".join(command), completed.returncode, completed.stderr[-4000:]))
    return completed.stdout.splitlines()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runner", required=True)
    parser.add_argument("--count", type=int, default=10000)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--batch")
    parser.add_argument("--out", default="http-difffuzz-out")
    args = parser.parse_args()

    temporary = tempfile.TemporaryDirectory(prefix="iron_http_diff_")
    batch = args.batch or os.path.join(temporary.name, "messages.hex")
    if not args.batch:
        with open(batch, "w", encoding="ascii") as stream:
            subprocess.run([sys.executable, os.path.join(HERE, "gen.py"),
                            str(args.count), str(args.seed)],
                           stdout=stream, check=True)
    documents = [line for line in open(batch, encoding="ascii").read().splitlines()
                 if line]
    iron = run([args.runner, "iron"], batch)
    pico = run([args.runner, "pico"], batch)
    if len(iron) != len(documents) or len(pico) != len(documents):
        print("result count mismatch: {} documents, {} Iron, {} pico".format(
            len(documents), len(iron), len(pico)), file=sys.stderr)
        return 2

    mismatches = []
    for index, (subject, reference) in enumerate(zip(iron, pico)):
        if subject != reference:
            mismatches.append((index, subject, reference))
    if mismatches:
        os.makedirs(args.out, exist_ok=True)
        for report, (index, subject, reference) in enumerate(mismatches[:20]):
            with open(os.path.join(args.out, "{}.http".format(report)), "wb") as stream:
                stream.write(bytes.fromhex(documents[index]))
            with open(os.path.join(args.out, "{}.txt".format(report)), "w",
                      encoding="utf-8") as stream:
                stream.write("document {}\nIron: {}\npico: {}\n".format(
                    index, subject, reference))
            print("MISMATCH document {}: Iron {!r}, pico {!r}".format(
                index, subject, reference))
    accepted = sum(line.startswith("OK") for line in iron)
    print("Iron vs picohttpparser: {} messages, {} accepted, {} rejected, {} mismatches"
          .format(len(documents), accepted, len(documents) - accepted,
                  len(mismatches)))
    return 1 if mismatches else 0


if __name__ == "__main__":
    raise SystemExit(main())
