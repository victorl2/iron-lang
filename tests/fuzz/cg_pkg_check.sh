#!/bin/sh
# cg_pkg_check.sh IRON PKGDIR OUTDIR
# Copies the package PKGDIR to OUTDIR, builds it with `iron build`, runs
# target/<dir name> and compares its output with PKGDIR/expected.
iron=$1 pkg=$2 out=$3
name=$(basename "$pkg")
rm -rf "$out" && mkdir -p "$out" && cp -R "$pkg" "$out/" || exit 1
cd "$out/$name" || exit 1
if ! "$iron" build > build.log 2>&1; then
    cat build.log
    echo "cg_pkg_check: build failed"
    exit 1
fi
./target/"$name" > got.txt 2>&1
diff expected got.txt || { echo "cg_pkg_check: output differs"; exit 1; }
