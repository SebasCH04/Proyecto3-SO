#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
PORT=9101
WORKDIR=/tmp/aws-s3-funcionales
DATADIR="$WORKDIR/data"
LOCALDIR="$WORKDIR/local"
DOWNLOADDIR="$WORKDIR/download"
SERVER_LOG="$WORKDIR/server.log"

cleanup() {
    if [ -n "${SERVER_PID:-}" ] && kill -0 "$SERVER_PID" 2>/dev/null; then
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
    rm -rf "$WORKDIR"
}

trap cleanup EXIT INT TERM

rm -rf "$WORKDIR"
mkdir -p "$DATADIR" "$LOCALDIR/dir/sub" "$DOWNLOADDIR"

printf 'hola mundo\n' > "$LOCALDIR/archivo.txt"
printf 'uno\n' > "$LOCALDIR/dir/a.txt"
printf 'dos\n' > "$LOCALDIR/dir/sub/b.txt"

"$ROOT_DIR/bin/aws-s3_server" --host 127.0.0.1 --port "$PORT" --data "$DATADIR" \
    >"$SERVER_LOG" 2>&1 &
SERVER_PID=$!
sleep 1

run_client() {
    AWS_S3_HOST=127.0.0.1 AWS_S3_PORT=$PORT "$ROOT_DIR/bin/aws-s3" "$@"
}

run_client mb s3://funcional
run_client cp "$LOCALDIR/archivo.txt" s3://funcional/
run_client cp "$LOCALDIR/dir" s3://funcional/respaldo/ --recursive
run_client cp s3://funcional/archivo.txt "$DOWNLOADDIR/archivo.txt"
cmp "$LOCALDIR/archivo.txt" "$DOWNLOADDIR/archivo.txt"

BUCKETS_OUTPUT=$(run_client ls)
OBJECTS_OUTPUT=$(run_client ls s3://funcional/ --recursive)

printf '%s\n' "$BUCKETS_OUTPUT" | grep -q 'funcional'
printf '%s\n' "$OBJECTS_OUTPUT" | grep -q 'archivo.txt'
printf '%s\n' "$OBJECTS_OUTPUT" | grep -q 'respaldo/a.txt'
printf '%s\n' "$OBJECTS_OUTPUT" | grep -q 'respaldo/sub/b.txt'

run_client rm s3://funcional/respaldo --recursive
POST_DELETE=$(run_client ls s3://funcional/ --recursive)
printf '%s\n' "$POST_DELETE" | grep -q 'archivo.txt'
if printf '%s\n' "$POST_DELETE" | grep -q 'respaldo/'; then
    echo "Fallo: el prefijo respaldo no fue eliminado" >&2
    exit 1
fi

run_client rm s3://funcional/archivo.txt
run_client rb s3://funcional

echo "Pruebas funcionales completadas correctamente."
