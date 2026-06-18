#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TMP="${TMPDIR:-/tmp}/aws-s3-test-$$"
PORT=$((20000 + ($$ % 20000)))
SERVER_PID=

cleanup() {
    if [ -n "$SERVER_PID" ]; then
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
    rm -rf "$TMP"
}
trap cleanup EXIT INT TERM

mkdir -p "$TMP/source/sub" "$TMP/download"
printf 'contenido de texto\n' > "$TMP/source/texto con espacios.txt"
dd if=/dev/urandom of="$TMP/source/sub/binario.dat" bs=1024 count=100 status=none

"$ROOT/bin/aws-s3_server" --host 127.0.0.1 --port "$PORT" \
    >"$TMP/server.out" 2>"$TMP/server.err" &
SERVER_PID=$!

attempt=0
while ! "$ROOT/bin/aws-s3" --port "$PORT" ls >/dev/null 2>&1; do
    attempt=$((attempt + 1))
    if [ "$attempt" -ge 30 ]; then
        cat "$TMP/server.err" >&2
        exit 1
    fi
    sleep 0.1
done

"$ROOT/bin/aws-s3" --port "$PORT" mb s3://prueba
"$ROOT/bin/aws-s3" --port "$PORT" cp "$TMP/source" s3://prueba/base/ --recursive
if "$ROOT/bin/aws-s3" --port "$PORT" rb s3://prueba >/dev/null 2>&1; then
    echo "rb eliminó un bucket no vacío sin --force" >&2
    exit 1
fi
"$ROOT/bin/aws-s3" --port "$PORT" cp s3://prueba/base/ "$TMP/download" --recursive

cmp "$TMP/source/texto con espacios.txt" "$TMP/download/texto con espacios.txt"
cmp "$TMP/source/sub/binario.dat" "$TMP/download/sub/binario.dat"

"$ROOT/bin/aws-s3" --port "$PORT" sync "$TMP/source" s3://prueba/sync/ --delete
rm "$TMP/source/texto con espacios.txt"
"$ROOT/bin/aws-s3" --port "$PORT" sync "$TMP/source" s3://prueba/sync/ --delete

LIST=$("$ROOT/bin/aws-s3" --port "$PORT" ls s3://prueba/sync/ --recursive)
printf '%s\n' "$LIST" | grep 'sync/sub/binario.dat' >/dev/null
if printf '%s\n' "$LIST" | grep 'texto con espacios.txt' >/dev/null; then
    echo "sync --delete no eliminó el objeto sobrante" >&2
    exit 1
fi

mkdir -p "$TMP/restored"
printf 'sobrante\n' > "$TMP/restored/sobrante.txt"
"$ROOT/bin/aws-s3" --port "$PORT" sync \
    s3://prueba/sync/ "$TMP/restored" --delete
cmp "$TMP/source/sub/binario.dat" "$TMP/restored/sub/binario.dat"
if [ -e "$TMP/restored/sobrante.txt" ]; then
    echo "sync S3->local --delete no eliminó el archivo sobrante" >&2
    exit 1
fi

mkdir -p "$TMP/move-local/nested"
printf 'mover\n' > "$TMP/move-local/nested/item.txt"
"$ROOT/bin/aws-s3" --port "$PORT" mv \
    "$TMP/move-local" s3://prueba/move-local/ --recursive
if [ -e "$TMP/move-local" ]; then
    echo "mv local recursivo no eliminó el árbol de origen" >&2
    exit 1
fi

"$ROOT/bin/aws-s3" --port "$PORT" mv \
    s3://prueba/sync/sub/binario.dat s3://prueba/movido.dat
"$ROOT/bin/aws-s3" --port "$PORT" rm s3://prueba/base/ --recursive
"$ROOT/bin/aws-s3" --port "$PORT" rb s3://prueba --force

echo "integration: OK"
