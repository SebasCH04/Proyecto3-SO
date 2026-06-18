#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
PORT=9102
WORKDIR=/tmp/aws-s3-validacion
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

start_server() {
    "$ROOT_DIR/bin/aws-s3_server" --host 127.0.0.1 --port "$PORT" --data "$DATADIR" \
        >"$SERVER_LOG" 2>&1 &
    SERVER_PID=$!
    sleep 1
}

stop_server() {
    if [ -n "${SERVER_PID:-}" ] && kill -0 "$SERVER_PID" 2>/dev/null; then
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
    SERVER_PID=
}

trap cleanup EXIT INT TERM

rm -rf "$WORKDIR"
mkdir -p "$DATADIR" "$LOCALDIR" "$DOWNLOADDIR"

run_client() {
    AWS_S3_HOST=127.0.0.1 AWS_S3_PORT=$PORT "$ROOT_DIR/bin/aws-s3" "$@"
}

dd if=/dev/zero of="$LOCALDIR/grande.bin" bs=1024 count=80 status=none
printf 'ABCDEF' > "$LOCALDIR/objeto.txt"
ln -s "$LOCALDIR/objeto.txt" "$LOCALDIR/enlace.txt"

start_server

run_client mb s3://validacion
if run_client mb s3://validacion >/dev/null 2>&1; then
    echo "Fallo: se permitió crear un bucket duplicado" >&2
    exit 1
fi

run_client cp "$LOCALDIR/grande.bin" s3://validacion/grande.bin
run_client cp "$LOCALDIR/objeto.txt" s3://validacion/objeto.txt

if run_client cp "$LOCALDIR/enlace.txt" s3://validacion/enlace.txt >/dev/null 2>&1; then
    echo "Fallo: se permitió copiar un enlace simbólico" >&2
    exit 1
fi

if run_client rb s3://validacion >/dev/null 2>&1; then
    echo "Fallo: se permitió borrar un bucket no vacío sin --force" >&2
    exit 1
fi

SIZE_BEFORE=$(wc -c < "$DATADIR/validacion.s3b")
printf '123456' > "$LOCALDIR/objeto.txt"
run_client cp "$LOCALDIR/objeto.txt" s3://validacion/objeto.txt
SIZE_SAME=$(wc -c < "$DATADIR/validacion.s3b")

printf '12345678901234567890' > "$LOCALDIR/objeto.txt"
run_client cp "$LOCALDIR/objeto.txt" s3://validacion/objeto.txt
SIZE_AFTER=$(wc -c < "$DATADIR/validacion.s3b")

run_client cp s3://validacion/grande.bin "$DOWNLOADDIR/grande.bin"
cmp "$LOCALDIR/grande.bin" "$DOWNLOADDIR/grande.bin"

stop_server
start_server

run_client cp s3://validacion/objeto.txt "$DOWNLOADDIR/objeto.txt"
cmp "$LOCALDIR/objeto.txt" "$DOWNLOADDIR/objeto.txt"

echo "tamano_bucket_antes=$SIZE_BEFORE"
echo "tamano_bucket_mismo_tamano=$SIZE_SAME"
echo "tamano_bucket_tamano_distinto=$SIZE_AFTER"
echo "Pruebas de validacion completadas correctamente."
