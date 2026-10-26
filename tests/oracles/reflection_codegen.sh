#!/usr/bin/env bash
set -euo pipefail

IRONC=${1:?usage: reflection_codegen.sh /path/to/ironc /path/to/source.iron}
SOURCE=${2:?usage: reflection_codegen.sh /path/to/ironc /path/to/source.iron}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

cd "$WORK"
"$IRONC" build --emit-c "$SOURCE"
C_SOURCE="$WORK/.iron-build/reflection_fields.c"

test -f "$C_SOURCE"
grep -qF 'iron_string_from_literal("id", 2)' "$C_SOURCE"
grep -qF 'iron_string_from_literal("name", 4)' "$C_SOURCE"
grep -qE '\.id([;[:space:]=])' "$C_SOURCE"
grep -qE '\.count([;[:space:]=])' "$C_SOURCE"

if grep -qE 'fields\(|__iron_fields_|reflected_field|field_descriptor' "$C_SOURCE"; then
    echo "generated C contains a run-time reflection artifact" >&2
    exit 1
fi
